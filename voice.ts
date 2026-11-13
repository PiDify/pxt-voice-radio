/**
 * Live push-to-talk voice over the micro:bit radio (micro:bit V2 only).
 *
 * The extension owns the radio: use voice.send / voice.readPacket /
 * voice.onPacket for your own short packets instead of the radio blocks.
 * In the simulator these functions do nothing.
 */
//% color=#C2185B weight=40 icon="" block="Voice radio"
namespace voice {
    /**
     * Start the radio on a group (0-255) and the voice player. Call once, first.
     */
    //% shim=voice::init
    export function init(group: number): void { }

    /**
     * Send one of your own packets (1-32 bytes, first byte must not be 0xA7).
     */
    //% shim=voice::send
    export function send(packet: Buffer): void { }

    /**
     * Take the next received packet; empty buffer when there is none.
     */
    //% shim=voice::readPacket
    export function readPacket(): Buffer { return pins.createBuffer(0) }

    /**
     * Run code when packets arrive (then read them with readPacket).
     */
    //% shim=voice::onPacket
    export function onPacket(body: () => void): void { }

    /**
     * Start sending the microphone live. id: this micro:bit's id (0-65535).
     */
    //% shim=voice::startTalking
    export function startTalking(id: number): void { }

    /**
     * Stop sending the microphone.
     */
    //% shim=voice::stopTalking
    export function stopTalking(): void { }

    /**
     * Only play voice coming from this talker id (-1 = play nothing).
     */
    //% shim=voice::setPeer
    export function setPeer(id: number): void { }

    /**
     * True while voice is being received and played.
     */
    //% shim=voice::isPlaying
    export function isPlaying(): boolean { return false }

    /**
     * Microphone loudness while talking, 0-255 (for a level meter).
     */
    //% shim=voice::micLevel
    export function micLevel(): number { return 0 }
}
