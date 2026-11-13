/*
 * voice-radio: live push-to-talk voice between two micro:bit V2 boards.
 *
 *   talker:   microphone -> megaphone effect -> 4-bit IMA ADPCM -> radio
 *   listener: radio -> ADPCM decode -> small jitter buffer -> speaker
 *
 * About 5.5 kHz audio, ~115 radio packets per second, ~0.1-0.2 s delay.
 * The extension owns the radio, so it also carries the program's own short
 * packets (send / readPacket / onPacket). Don't use the normal radio blocks
 * in the same program.
 *
 * MIT licence.
 */
#ifndef VOICE_HOST_TEST
#include "pxt.h"
#include "MicroBit.h"
#include "StreamSplitter.h"
#include "Mixer2.h"
using namespace pxt;
#else
#include <stdint.h>
#include <string.h>
#define DATASTREAM_FORMAT_8BIT_UNSIGNED 1
#endif

#define VOICE_MAGIC        0xA7
#define VOICE_PKT_LEN      32
#define VOICE_HDR_LEN      8
#define VOICE_DATA_LEN     (VOICE_PKT_LEN - VOICE_HDR_LEN)   // 24 bytes
#define VOICE_PKT_SAMPLES  (VOICE_DATA_LEN * 2)              // 48 samples
#define VOICE_TARGET_RATE  5500
#define VOICE_EVT_ID       7350
#define VOICE_EVT_PACKET   1
#define RX_RING            2048          // power of two
#define RX_MASK            (RX_RING - 1)
#define RX_PRIME           200           // samples buffered before playing (~36 ms)
#define RX_MAX_BACKLOG     1100          // keep the delay under ~0.2 s
#define TX_QUEUE           8
#define PKT_QUEUE          8

namespace voice {

// ===================== portable part (also host-tested) ======================

static const int16_t stepTable[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767};
static const int8_t indexTable[16] = {-1, -1, -1, -1, 2, 4, 6, 8,
                                      -1, -1, -1, -1, 2, 4, 6, 8};

static inline int clamp16(int v) {
    return v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
}

struct Adpcm {
    int pred;
    int index;
};

static uint8_t adpcmEncode(Adpcm &st, int sample) {
    int step = stepTable[st.index];
    int diff = sample - st.pred;
    uint8_t code = 0;
    if (diff < 0) {
        code = 8;
        diff = -diff;
    }
    int delta = step >> 3;
    if (diff >= step) { code |= 4; diff -= step; delta += step; }
    step >>= 1;
    if (diff >= step) { code |= 2; diff -= step; delta += step; }
    step >>= 1;
    if (diff >= step) { code |= 1; delta += step; }
    st.pred = clamp16((code & 8) ? st.pred - delta : st.pred + delta);
    st.index += indexTable[code];
    if (st.index < 0) st.index = 0;
    if (st.index > 88) st.index = 88;
    return code;
}

static int adpcmDecode(Adpcm &st, uint8_t code) {
    int step = stepTable[st.index];
    int delta = step >> 3;
    if (code & 4) delta += step;
    if (code & 2) delta += step >> 1;
    if (code & 1) delta += step >> 2;
    st.pred = clamp16((code & 8) ? st.pred - delta : st.pred + delta);
    st.index += indexTable[code & 15];
    if (st.index < 0) st.index = 0;
    if (st.index > 88) st.index = 88;
    return st.pred;
}

// one sample of any CODAL stream format -> signed 16-bit range
static int readSample(const uint8_t *p, int i, int fmt) {
    int bps = (fmt + 1) / 2;
    if (bps < 1 || bps > 4) return 0;
    const uint8_t *q = p + i * bps;
    uint32_t u = 0;
    for (int k = 0; k < bps; k++) u |= ((uint32_t)q[k]) << (8 * k);
    int bits = bps * 8;
    int32_t s;
    if (fmt & 1) {                                   // odd formats are unsigned
        s = (int32_t)(u - (1u << (bits - 1)));
    } else {                                         // signed: sign-extend
        s = (int32_t)(u << (32 - bits)) >> (32 - bits);
    }
    return bits >= 16 ? (s >> (bits - 16)) : (s << (16 - bits));
}

// Downsamples to ~5.5 kHz and gives the "police megaphone" sound:
// thin (low frequencies removed), loud (automatic gain), slightly distorted
// (soft clipping), and quiet between words (noise gate).
struct Megaphone {
    int decimN, decimAcc, decimCount;
    int prevIn, hp, peak, noiseQ8, gainQ8, gateQ8;

    void begin(int inRate) {
        decimN = (inRate + VOICE_TARGET_RATE / 2) / VOICE_TARGET_RATE;
        if (decimN < 1) decimN = 1;
        decimAcc = 0;
        decimCount = 0;
        prevIn = 0;
        hp = 0;
        peak = 0;
        noiseQ8 = 3000 << 8;     // starts high, drops to the real background fast
        gainQ8 = 4 << 8;
        gateQ8 = 0;
    }
    int outRate(int inRate) { return inRate / decimN; }

    int process(int x) {
        hp = (190 * (hp + x - prevIn)) >> 8;           // high-pass ~250 Hz
        prevIn = x;
        int a = hp < 0 ? -hp : hp;
        if (a > peak) peak = a;                         // level follower:
        else peak -= (peak >> 11) + 1;                  // instant up, ~0.4 s down
        if (peak < 0) peak = 0;
        int noise = noiseQ8 >> 8;                       // background estimate
        if (peak < noise) noiseQ8 = peak << 8;
        else noiseQ8 += ((peak << 8) - noiseQ8) >> 14;
        noise = noiseQ8 >> 8;
        int gateTarget = (peak > noise * 3 + 80) ? 256 : 16;
        gateQ8 += (gateTarget - gateQ8) >> 4;
        int want = peak > 64 ? (22000 * 256) / peak : (48 << 8);
        if (want > (48 << 8)) want = 48 << 8;
        if (want < 256) want = 256;
        gainQ8 += (want - gainQ8) >> 7;
        int v = (hp * gainQ8) >> 8;
        v = (v * gateQ8) >> 8;
        if (v > 14000) v = 14000 + ((v - 14000) >> 2);  // soft clip
        else if (v < -14000) v = -14000 + ((v + 14000) >> 2);
        return clamp16(v);
    }

    // feed one microphone sample; true when an output sample is ready
    bool feed(int x, int &out) {
        decimAcc += x;
        if (++decimCount < decimN) return false;
        x = decimAcc / decimN;
        decimAcc = 0;
        decimCount = 0;
        out = process(x);
        return true;
    }
    int level() { int l = peak >> 7; return l > 255 ? 255 : l; }
};

// packs samples into 32-byte radio packets:
// [A7, idLo, idHi, seq, rate/100, predLo, predHi, index, 24 bytes = 48 samples]
struct Encoder {
    Adpcm st;
    int n, id, rate100;
    uint8_t seq;
    uint8_t pkt[VOICE_PKT_LEN];

    void begin(int id_, int rate) {
        st.pred = 0;
        st.index = 0;
        n = 0;
        seq = 0;
        id = id_;
        rate100 = (rate + 50) / 100;
        if (rate100 > 255) rate100 = 255;
    }
    bool add(int sample) {                  // true when pkt is complete
        if (n == 0) {
            pkt[0] = VOICE_MAGIC;
            pkt[1] = id & 255;
            pkt[2] = (id >> 8) & 255;
            pkt[3] = seq++;
            pkt[4] = rate100;
            pkt[5] = st.pred & 255;
            pkt[6] = (st.pred >> 8) & 255;
            pkt[7] = st.index;
        }
        uint8_t code = adpcmEncode(st, sample);
        int b = VOICE_HDR_LEN + (n >> 1);
        if (n & 1) pkt[b] |= code << 4;
        else pkt[b] = code;
        if (++n == VOICE_PKT_SAMPLES) {
            n = 0;
            return true;
        }
        return false;
    }
};

static int decodePacket(const uint8_t *d, int16_t *out) {
    Adpcm st;
    st.pred = (int16_t)(d[5] | (d[6] << 8));
    st.index = d[7] > 88 ? 88 : d[7];
    for (int i = 0; i < VOICE_DATA_LEN; i++) {
        uint8_t b = d[VOICE_HDR_LEN + i];
        out[2 * i] = adpcmDecode(st, b & 15);
        out[2 * i + 1] = adpcmDecode(st, b >> 4);
    }
    return VOICE_PKT_SAMPLES;
}

static inline int packetId(const uint8_t *d) { return d[1] | (d[2] << 8); }
static inline int packetRate(const uint8_t *d) { return d[4] * 100; }

// ===================== micro:bit part (CODAL) ======================
#ifndef VOICE_HOST_TEST

static bool inited = false;
static int peerId = -1;                   // listener: only accept this talker
static volatile uint32_t lastVoiceMs = 0;

// ---- outgoing voice packets (filled by the mic, sent by a fiber) ----
static uint8_t txBuf[TX_QUEUE][VOICE_PKT_LEN];
static volatile int txHead = 0, txTail = 0;

static void queueTx(const uint8_t *pkt) {
    int next = (txHead + 1) % TX_QUEUE;
    if (next == txTail) return;           // full: drop
    memcpy(txBuf[txHead], pkt, VOICE_PKT_LEN);
    txHead = next;
}

static void txFiber() {
    while (true) {
        if (txTail != txHead) {
            uBit.radio.datagram.send(txBuf[txTail], VOICE_PKT_LEN);
            txTail = (txTail + 1) % TX_QUEUE;
        } else {
            codal::fiber_sleep(2);
        }
    }
}

// ---- app packets (anything that isn't voice) for the TypeScript side ----
static uint8_t pktData[PKT_QUEUE][VOICE_PKT_LEN];
static int pktLen[PKT_QUEUE];
static volatile int pktHead = 0, pktTail = 0;

// ---- microphone -> radio ----
class VoiceTx : public DataSink {
  public:
    DataSource &src;
    volatile bool active;
    Megaphone mega;
    Encoder enc;

    VoiceTx(DataSource &s) : src(s) {
        active = false;
        src.connect(*this);
    }
    void begin(int id) {
        active = false;
        float r = src.getSampleRate();
        int rate = r > 1000 ? (int)r : 11000;
        mega.begin(rate);
        enc.begin(id, mega.outRate(rate));
        active = true;
    }
    void end() {
        if (!active) return;
        active = false;
        if (enc.n != 0) {                 // send the last few samples too
            while (!enc.add(0)) {}
            queueTx(enc.pkt);
        }
    }
    virtual int pullRequest() {
        ManagedBuffer b = src.pull();
        if (!active || b.length() == 0) return DEVICE_OK;
        int fmt = src.getFormat();
        int bps = (fmt + 1) / 2;
        if (bps < 1 || bps > 4) return DEVICE_OK;
        int n = b.length() / bps;
        uint8_t *p = b.getBytes();
        for (int i = 0; i < n; i++) {
            int y;
            if (mega.feed(readSample(p, i, fmt), y)) {
                if (enc.add(y)) queueTx(enc.pkt);
            }
        }
        return DEVICE_OK;
    }
};

// ---- radio -> speaker ----
class VoiceRx : public DataSource {
  public:
    DataSink *downStream;
    volatile int head, tail;
    volatile bool playing;
    int rate;
    uint8_t ring[RX_RING];

    VoiceRx() {
        downStream = NULL;
        head = 0;
        tail = 0;
        playing = false;
        rate = VOICE_TARGET_RATE;
    }
    int count() { return (head - tail) & RX_MASK; }

    virtual ManagedBuffer pull() {
        int avail = count();
        if (avail <= 0) {
            playing = false;              // ran dry: wait for the next packets
            return ManagedBuffer();
        }
        int n = avail > 256 ? 256 : avail;
        ManagedBuffer b(n);
        uint8_t *d = b.getBytes();
        int t = tail;
        for (int i = 0; i < n; i++) {
            d[i] = ring[t];
            t = (t + 1) & RX_MASK;
        }
        tail = t;
        if (downStream != NULL) downStream->pullRequest();
        return b;
    }
    virtual void connect(DataSink &sink) { downStream = &sink; }
    virtual bool isConnected() { return downStream != NULL; }
    virtual void disconnect() { downStream = NULL; }
    virtual int getFormat() { return DATASTREAM_FORMAT_8BIT_UNSIGNED; }
    virtual int setFormat(int format) { return DEVICE_NOT_SUPPORTED; }
    virtual float getSampleRate() { return (float)rate; }

    void addSamples(const int16_t *s, int n) {
        if (count() > RX_MAX_BACKLOG) return;     // too far behind: skip
        int h = head;
        for (int i = 0; i < n; i++) {
            int next = (h + 1) & RX_MASK;
            if (next == tail) break;
            int v = (s[i] >> 8) + 128;
            ring[h] = v < 0 ? 0 : (v > 255 ? 255 : v);
            h = next;
        }
        head = h;
        if (!playing && count() >= RX_PRIME && downStream != NULL) {
            playing = true;
            downStream->pullRequest();
        }
    }
};

static SplitterChannel *micChan = NULL;
static VoiceTx *tx = NULL;
static VoiceRx *rx = NULL;
static MixerChannel *mixChan = NULL;

static void onRadio(MicroBitEvent) {
    bool gotApp = false;
    while (true) {
        PacketBuffer p = uBit.radio.datagram.recv();
        if (p == PacketBuffer::EmptyPacket) break;
        int len = p.length();
        uint8_t *d = p.getBytes();
        if (len == VOICE_PKT_LEN && d[0] == VOICE_MAGIC) {
            if (rx == NULL || peerId < 0 || packetId(d) != peerId) continue;
            int r = packetRate(d);
            if (r >= 2000 && r <= 16000 && r != rx->rate) {
                rx->rate = r;
                if (mixChan != NULL) mixChan->setSampleRate((float)r);
            }
            int16_t s[VOICE_PKT_SAMPLES];
            decodePacket(d, s);
            rx->addSamples(s, VOICE_PKT_SAMPLES);
            lastVoiceMs = (uint32_t)codal::system_timer_current_time();
        } else if (len > 0 && len <= VOICE_PKT_LEN) {
            int next = (pktHead + 1) % PKT_QUEUE;
            if (next == pktTail) pktTail = (pktTail + 1) % PKT_QUEUE;   // drop oldest
            memcpy(pktData[pktHead], d, len);
            pktLen[pktHead] = len;
            pktHead = next;
            gotApp = true;
        }
    }
    if (gotApp) {
        MicroBitEvent evt(VOICE_EVT_ID, VOICE_EVT_PACKET);   // wakes onPacket
    }
}

/**
 * Start the radio (on this group) and the voice player. Call once, first.
 */
//%
void init(int group) {
    if (inited) return;
    inited = true;
    uBit.radio.enable();
    uBit.radio.setGroup(group);
    uBit.radio.setTransmitPower(7);
    MicroBitAudio::requestActivation();
    uBit.audio.setVolume(255);            // master volume at maximum
    rx = new VoiceRx();
    mixChan = uBit.audio.mixer.addChannel(*rx, (float)VOICE_TARGET_RATE, 255);
    mixChan->setVolume(1023.0f);
    uBit.messageBus.listen(MICROBIT_ID_RADIO, MICROBIT_RADIO_EVT_DATAGRAM, onRadio);
    codal::create_fiber(txFiber);
    while (!(uBit.radio.datagram.recv() == PacketBuffer::EmptyPacket)) {}
}

/**
 * Send one of the program's own packets (up to 32 bytes).
 */
//%
void send(Buffer packet) {
    if (!inited || packet == NULL) return;
    int len = packet->length;
    if (len <= 0) return;
    if (len > VOICE_PKT_LEN) len = VOICE_PKT_LEN;
    uBit.radio.datagram.send(packet->data, len);
}

/**
 * Take the next received program packet (empty buffer if none).
 */
//%
Buffer readPacket() {
    if (pktHead == pktTail) return mkBuffer(NULL, 0);
    Buffer b = mkBuffer(pktData[pktTail], pktLen[pktTail]);
    pktTail = (pktTail + 1) % PKT_QUEUE;
    return b;
}

/**
 * Run code when program packets arrive (read them with readPacket).
 */
//%
void onPacket(Action body) {
    registerWithDal(VOICE_EVT_ID, VOICE_EVT_PACKET, body);
}

/**
 * Start sending the microphone live. id = this micro:bit's id (0..65535).
 */
//%
void startTalking(int id) {
    if (!inited) return;
    if (micChan == NULL) {
        micChan = uBit.audio.splitter->createChannel();
        tx = new VoiceTx(*micChan);
    }
    tx->begin(id & 0xffff);
    micChan->dataWanted(DATASTREAM_WANTED);
}

/**
 * Stop sending the microphone.
 */
//%
void stopTalking() {
    if (tx != NULL) tx->end();
    if (micChan != NULL) micChan->dataWanted(DATASTREAM_DONT_CARE);
}

/**
 * Only play voice from this talker id (-1 = play nothing).
 */
//%
void setPeer(int id) {
    peerId = id < 0 ? -1 : (id & 0xffff);
}

/**
 * True while voice is being received and played.
 */
//%
bool isPlaying() {
    if (rx == NULL) return false;
    uint32_t now = (uint32_t)codal::system_timer_current_time();
    return rx->count() > 0 || (now - lastVoiceMs) < 300;
}

/**
 * Microphone loudness while talking, 0..255 (for a level meter).
 */
//%
int micLevel() {
    if (tx == NULL || !tx->active) return 0;
    return tx->mega.level();
}

#endif  // VOICE_HOST_TEST
} // namespace voice
