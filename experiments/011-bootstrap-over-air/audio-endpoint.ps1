<#
    Windows audio endpoint level control, shared by the Experiment 011 rigs.

    WHY THIS FILE EXISTS AT ALL

    The first board run of Experiment 011 recorded every capture at a peak of
    exactly 0.0 dB. That is not a strong signal, it is a CLIPPED one: the ADC
    ran out of range and the tone tops were flattened, which manufactures
    harmonics the demodulator then has to decide against. A clipped capture is
    an instrument fault in the same way a silent one is, and it is the more
    dangerous of the two because it looks like the channel is working.

    The retained corpus this experiment is compared against was recorded at
    -0.7 dB (008) and -3.5 dB (003) peak. So the rig aims for that window and
    says so in the log, rather than leaving the level wherever the machine
    happened to be.

    THE VTABLE SLOT THAT MUTES THE MACHINE

    IAudioEndpointVolume puts GetMasterVolumeLevel -- DECIBELS -- at slot 6 and
    GetMasterVolumeLevelScalar at slot 7. Declaring only the scalar getter puts
    it on slot 6, so a "scalar" read returns a dB value: about -20 at a normal
    level, which clamps to 0. Restoring that 0 mutes the endpoint. The dB getter
    is therefore declared and never called, purely to hold its slot.
#>

Add-Type -TypeDefinition @'
using System.Runtime.InteropServices;

[Guid("5CDF2C82-841E-4546-9722-0CF74078229A"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IAudioEndpointVolume {
  int RegisterControlChangeNotify();
  int UnregisterControlChangeNotify();
  int GetChannelCount();
  int SetMasterVolumeLevel();
  int SetMasterVolumeLevelScalar(float level, System.Guid ctx);
  int GetMasterVolumeLevelDb(out float level);      // slot 6: dB, never called
  int GetMasterVolumeLevelScalar(out float level);  // slot 7: the 0..1 scalar
}
[Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IMMDevice {
  int Activate(ref System.Guid id, int ctx, System.IntPtr p, out IAudioEndpointVolume o);
}
[Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IMMDeviceEnumerator {
  int NotImplemented();
  int GetDefaultAudioEndpoint(int flow, int role, out IMMDevice dev);
}
[ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")] class MMDeviceEnumeratorComObject { }

public class Ep {
  public const int Render = 0;   // eRender:  speakers
  public const int Capture = 1;  // eCapture: microphones

  static IAudioEndpointVolume Endpoint(int flow) {
    IMMDevice dev; IAudioEndpointVolume ep;
    var e = (IMMDeviceEnumerator)(new MMDeviceEnumeratorComObject());
    e.GetDefaultAudioEndpoint(flow, 1, out dev);
    var iid = typeof(IAudioEndpointVolume).GUID;
    dev.Activate(ref iid, 23, System.IntPtr.Zero, out ep);
    return ep;
  }
  public static float Level(int flow) {
    float v; Endpoint(flow).GetMasterVolumeLevelScalar(out v); return v;
  }
  public static void Set(int flow, float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    Endpoint(flow).SetMasterVolumeLevelScalar(v, System.Guid.Empty);
  }
}
'@ -ErrorAction SilentlyContinue
