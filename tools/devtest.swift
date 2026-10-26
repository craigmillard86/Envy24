// Channel test for the Envy24 device.
//
//   devtest play [rate]          tone on output 1, then 2, 3, 4 (n beeps on output n)
//   devtest record [secs] [rate] per-channel peak level of inputs 1-4, once a second
//   devtest rate [hz]            show or set the device's nominal sample rate
//
// Build: swiftc -O tools/devtest.swift -o build/devtest

import AudioToolbox
import CoreAudio
import Foundation

let deviceNames = ["M-Audio Delta 66", "Envy24"]
let channels: UInt32 = 4

func fail(_ msg: String) -> Never {
    FileHandle.standardError.write((msg + "\n").data(using: .utf8)!)
    exit(1)
}

func check(_ status: OSStatus, _ what: String) {
    if status != noErr { fail("\(what) failed: \(status)") }
}

func deviceID() -> AudioObjectID {
    var addr = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyTranslateUIDToDevice,
                                          mScope: kAudioObjectPropertyScopeGlobal,
                                          mElement: kAudioObjectPropertyElementMain)
    var uid: CFString = deviceUID()
    var id = AudioObjectID(0)
    var size = UInt32(MemoryLayout<AudioObjectID>.size)
    check(AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &addr,
                                     UInt32(MemoryLayout<CFString>.size), &uid, &size, &id), "device ID")
    return id
}

func nominalRate(set newRate: Double?) {
    let id = deviceID()
    var addr = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyNominalSampleRate,
                                          mScope: kAudioObjectPropertyScopeGlobal,
                                          mElement: kAudioObjectPropertyElementMain)
    var rate = Float64(0)
    var size = UInt32(MemoryLayout<Float64>.size)
    if var r = newRate {
        check(AudioObjectSetPropertyData(id, &addr, 0, nil, size, &r), "set sample rate")
        let end = Date().addingTimeInterval(1)
        while Date() < end { CFRunLoopRunInMode(.defaultMode, 0.05, false) }
    }
    check(AudioObjectGetPropertyData(id, &addr, 0, nil, &size, &rate), "get sample rate")
    print("nominal sample rate: \(Int(rate)) Hz")
}

// The driver names the device after the board; older builds called it "Envy24".
func deviceUID() -> CFString {
    var addr = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyDevices,
                                          mScope: kAudioObjectPropertyScopeGlobal,
                                          mElement: kAudioObjectPropertyElementMain)
    var size: UInt32 = 0
    check(AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size), "device list size")
    var ids = [AudioObjectID](repeating: 0, count: Int(size) / MemoryLayout<AudioObjectID>.size)
    check(AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size, &ids), "device list")

    for id in ids {
        var nameAddr = AudioObjectPropertyAddress(mSelector: kAudioObjectPropertyName,
                                                  mScope: kAudioObjectPropertyScopeGlobal,
                                                  mElement: kAudioObjectPropertyElementMain)
        var cfName: Unmanaged<CFString>?
        var nameSize = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
        guard AudioObjectGetPropertyData(id, &nameAddr, 0, nil, &nameSize, &cfName) == noErr,
              let devName = cfName?.takeRetainedValue() as String?, deviceNames.contains(devName) else { continue }

        var uidAddr = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyDeviceUID,
                                                 mScope: kAudioObjectPropertyScopeGlobal,
                                                 mElement: kAudioObjectPropertyElementMain)
        var uid: Unmanaged<CFString>?
        var uidSize = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
        check(AudioObjectGetPropertyData(id, &uidAddr, 0, nil, &uidSize, &uid), "device UID")
        return uid!.takeRetainedValue()
    }
    fail("no audio device named \(deviceNames.joined(separator: " or "))")
}

func format(rate: Double) -> AudioStreamBasicDescription {
    AudioStreamBasicDescription(mSampleRate: rate,
                                mFormatID: kAudioFormatLinearPCM,
                                mFormatFlags: kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked,
                                mBytesPerPacket: 4 * channels, mFramesPerPacket: 1,
                                mBytesPerFrame: 4 * channels, mChannelsPerFrame: channels,
                                mBitsPerChannel: 32, mReserved: 0)
}

// --- play ------------------------------------------------------------------

final class Player {
    let rate: Double
    var frame: Int = 0
    let segment: Int          // frames per output channel
    init(rate: Double) { self.rate = rate; segment = Int(rate * 2.5) }

    var totalFrames: Int { segment * Int(channels) }

    // n beeps of 1 kHz on output n, during segment n
    func fill(_ buf: UnsafeMutablePointer<Float>, frames: Int) {
        for i in 0..<frames {
            let f = frame + i
            let ch = f / segment
            let t = f % segment
            let beepLen = Int(rate * 0.25), gap = Int(rate * 0.2)
            let beep = t / (beepLen + gap)
            let on = ch < Int(channels) && beep <= ch && (t % (beepLen + gap)) < beepLen
            let s: Float = on ? 0.3 * sinf(Float(2.0 * Double.pi * 1000.0 * Double(f) / rate)) : 0
            for c in 0..<Int(channels) { buf[i * Int(channels) + c] = (c == ch) ? s : 0 }
        }
        frame += frames
    }
}

func play(rate: Double) {
    var fmt = format(rate: rate)
    let player = Player(rate: rate)
    var queue: AudioQueueRef?
    let ctx = Unmanaged.passUnretained(player).toOpaque()

    check(AudioQueueNewOutput(&fmt, { ctx, q, buf in
        let p = Unmanaged<Player>.fromOpaque(ctx!).takeUnretainedValue()
        let frames = Int(buf.pointee.mAudioDataBytesCapacity) / (4 * Int(channels))
        p.fill(buf.pointee.mAudioData.assumingMemoryBound(to: Float.self), frames: frames)
        buf.pointee.mAudioDataByteSize = buf.pointee.mAudioDataBytesCapacity
        AudioQueueEnqueueBuffer(q, buf, 0, nil)
    }, ctx, nil, nil, 0, &queue), "AudioQueueNewOutput")

    var uid: CFString? = deviceUID()
    check(AudioQueueSetProperty(queue!, kAudioQueueProperty_CurrentDevice, &uid, UInt32(MemoryLayout<CFString?>.size)), "set device")

    for _ in 0..<3 {
        var buf: AudioQueueBufferRef?
        check(AudioQueueAllocateBuffer(queue!, 4096 * 4 * channels, &buf), "allocate buffer")
        player.fill(buf!.pointee.mAudioData.assumingMemoryBound(to: Float.self), frames: 4096)
        buf!.pointee.mAudioDataByteSize = 4096 * 4 * channels
        AudioQueueEnqueueBuffer(queue!, buf!, 0, nil)
    }

    print("Playing at \(Int(rate)) Hz: n beeps on output n, 1 to 4")
    check(AudioQueueStart(queue!, nil), "AudioQueueStart")
    var announced = -1
    while player.frame < player.totalFrames + Int(rate) {
        let ch = min(player.frame / player.segment, Int(channels) - 1)
        if ch != announced { print("  output \(ch + 1)"); announced = ch }
        CFRunLoopRunInMode(.defaultMode, 0.1, false)
    }
    AudioQueueStop(queue!, true)
    AudioQueueDispose(queue!, true)
}

// --- record ----------------------------------------------------------------

final class Meter {
    var peak = [Float](repeating: 0, count: Int(channels))
    var frames = 0
}

func record(seconds: Int, rate: Double) {
    var fmt = format(rate: rate)
    let meter = Meter()
    var queue: AudioQueueRef?
    let ctx = Unmanaged.passUnretained(meter).toOpaque()

    check(AudioQueueNewInput(&fmt, { ctx, q, buf, _, _, _ in
        let m = Unmanaged<Meter>.fromOpaque(ctx!).takeUnretainedValue()
        let data = buf.pointee.mAudioData.assumingMemoryBound(to: Float.self)
        let frames = Int(buf.pointee.mAudioDataByteSize) / (4 * Int(channels))
        for i in 0..<frames {
            for c in 0..<Int(channels) { m.peak[c] = max(m.peak[c], abs(data[i * Int(channels) + c])) }
        }
        m.frames += frames
        AudioQueueEnqueueBuffer(q, buf, 0, nil)
    }, ctx, nil, nil, 0, &queue), "AudioQueueNewInput")

    var uid: CFString? = deviceUID()
    check(AudioQueueSetProperty(queue!, kAudioQueueProperty_CurrentDevice, &uid, UInt32(MemoryLayout<CFString?>.size)), "set device")

    for _ in 0..<3 {
        var buf: AudioQueueBufferRef?
        check(AudioQueueAllocateBuffer(queue!, 4096 * 4 * channels, &buf), "allocate buffer")
        AudioQueueEnqueueBuffer(queue!, buf!, 0, nil)
    }

    print("Recording \(seconds) s at \(Int(rate)) Hz, peak per input in dBFS")
    print("  sec   in1     in2     in3     in4   frames")
    check(AudioQueueStart(queue!, nil), "AudioQueueStart")
    var total = 0
    var start: Date? = nil
    for s in 1...seconds {
        let end = Date().addingTimeInterval(1)
        while Date() < end { CFRunLoopRunInMode(.defaultMode, 0.05, false) }
        let db = meter.peak.map { $0 > 0 ? String(format: "%6.1f", 20 * log10($0)) : "  -inf" }
        print(String(format: "  %3d ", s) + db.joined(separator: "  ") + "  \(meter.frames)")
        // measure from the end of the first second, once the queue is in steady state
        if start == nil { start = Date() } else { total += meter.frames }
        meter.peak = [Float](repeating: 0, count: Int(channels))
        meter.frames = 0
    }
    if let start = start, seconds > 1 {
        print(String(format: "  measured rate: %.0f frames/s", Double(total) / Date().timeIntervalSince(start)))
    }
    AudioQueueStop(queue!, true)
    AudioQueueDispose(queue!, true)
}

// --- main ------------------------------------------------------------------

let args = CommandLine.arguments
switch args.count > 1 ? args[1] : "" {
case "play":
    play(rate: args.count > 2 ? Double(args[2])! : 44100)
case "record":
    record(seconds: args.count > 2 ? Int(args[2])! : 10, rate: args.count > 3 ? Double(args[3])! : 44100)
case "rate":
    nominalRate(set: args.count > 2 ? Double(args[2])! : nil)
default:
    fail("usage: devtest play [rate] | devtest record [secs] [rate] | devtest rate [hz]")
}
