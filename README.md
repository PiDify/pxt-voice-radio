# voice-radio

Live push-to-talk voice between two **micro:bit V2** boards over the built-in radio,
with a police-megaphone sound. Made for the Bit:Bot XL tilt racer.

* Talker: microphone -> megaphone effect (low cut, automatic gain, soft clipping,
  noise gate) -> 4-bit IMA ADPCM -> radio, about 5.5 kHz audio, ~115 packets/s.
* Listener: radio -> decode -> small jitter buffer -> speaker. Delay about 0.1-0.2 s.

The extension owns the radio, so it also carries your program's own short packets.
Don't use the normal **radio** blocks in the same program.

```typescript
voice.init(42)                       // same group on both micro:bits
voice.onPacket(function () {
    let p = voice.readPacket()
    while (p.length > 0) {
        // ... your packet ...
        p = voice.readPacket()
    }
})
voice.setPeer(1234)                  // listener: play voice from talker 1234
voice.startTalking(1234)             // talker: send the microphone live
voice.stopTalking()
```

## API

| function | what it does |
|---|---|
| `voice.init(group)` | start radio + voice player (call once) |
| `voice.send(buffer)` | send your own packet (1-32 bytes, first byte not 0xA7) |
| `voice.readPacket()` | next received packet, empty buffer if none |
| `voice.onPacket(handler)` | run code when packets arrive |
| `voice.startTalking(id)` / `voice.stopTalking()` | stream the microphone live |
| `voice.setPeer(id)` | listener only plays voice from this id (-1 = none) |
| `voice.isPlaying()` | true while voice is coming in |
| `voice.micLevel()` | microphone level 0-255 while talking |

Voice packets: `[0xA7, idLo, idHi, seq, rate/100, predLo, predHi, stepIndex, 24 bytes of 4-bit ADPCM]`.

MIT licence.
