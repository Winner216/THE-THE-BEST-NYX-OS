#ifndef WASAPI_H
#define WASAPI_H

// ─── Windows Core Audio / WASAPI Compatibility Layer ─────────────
// Implements the Windows Multimedia API (waveOut/waveIn) and
// Core Audio (WASAPI) session APIs so PE executables (like Telegram)
// can output and capture PCM audio through Nyx OS's AC'97 driver.
//
// Three tiers:
//   1. waveOut/waveIn  — the classic MME API (oldest, widest compat)
//   2. WASAPI          — modern session API (exclusive + shared mode)
//   3. Mixer API       — volume/mute control per line
//
// All backed by the kernel's audio subsystem (cpu/audio.c, cpu/ac97.c).

#include <stdint.h>

// ─── Basic Win32 types ──────────────────────────────────────────
// When included after winapi.h these are already defined.
#ifndef WINAPI_H
typedef unsigned int   UINT;
typedef unsigned int   DWORD;
typedef int            BOOL;
typedef unsigned char  BYTE;
typedef unsigned short WORD;
typedef void*          HANDLE;
typedef void*          LPVOID;
typedef const char*    LPCSTR;
typedef const void*    LPCVOID;
typedef char*          LPSTR;
typedef DWORD*         LPDWORD;
typedef long           LONG;
typedef unsigned long  ULONG;
typedef unsigned long  SIZE_T;
#define TRUE  1
#define FALSE 0
#endif

// Pointer-sized types (always 32-bit on i386)
#ifndef DWORD_PTR
typedef unsigned long DWORD_PTR;
#endif
#ifndef UINT_PTR
typedef unsigned long UINT_PTR;
#endif
#ifndef LONG_PTR
typedef long LONG_PTR;
#endif
#ifndef UINT64
typedef unsigned long long UINT64;
#endif

// ─── GUID ───────────────────────────────────────────────────────
typedef struct {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[8];
} GUID;
typedef GUID IID;
typedef GUID REFIID;

// ─── Error codes ────────────────────────────────────────────────
#define MMSYSERR_NOERROR      0
#define MMSYSERR_ERROR        1
#define MMSYSERR_BADDEVICEID  2
#define MMSYSERR_NOTENABLED   3
#define MMSYSERR_ALLOCATED    4
#define MMSYSERR_INVALHANDLE  5
#define MMSYSERR_NODRIVER     6
#define MMSYSERR_NOMEM        7
#define MMSYSERR_INVALPARAM   11
#define WAVERR_BADFORMAT      32
#define WAVERR_STILLPLAYING   33
#define WAVERR_UNPREPARED     34
#define WAVERR_SYNC           35

typedef UINT MMRESULT;

// ─── WAVEFORMATEX ───────────────────────────────────────────────
typedef struct {
    WORD  wFormatTag;       // WAVE_FORMAT_PCM = 1
    WORD  nChannels;        // 1 = mono, 2 = stereo
    DWORD nSamplesPerSec;   // e.g. 44100, 48000
    DWORD nAvgBytesPerSec;  // nBlockAlign * nSamplesPerSec
    WORD  nBlockAlign;      // nChannels * wBitsPerSample / 8
    WORD  wBitsPerSample;   // 8, 16, 24, 32
    WORD  cbSize;           // extra format bytes (0 for PCM)
} WAVEFORMATEX;
typedef const WAVEFORMATEX* LPCWAVEFORMATEX;

// Format tags
#define WAVE_FORMAT_PCM  1

// ─── WAVEHDR (buffer descriptor for waveOut/waveIn) ─────────────
typedef struct wavehdr_tag {
    LPSTR       lpData;          // pointer to locked data buffer
    DWORD       dwBufferLength;  // length of data buffer
    DWORD       dwBytesRecorded; // bytes actually recorded (waveIn)
    DWORD       dwUser;          // user data
    DWORD       dwFlags;         // WHDR_DONE, WHDR_PREPARED, etc.
    DWORD       dwLoops;         // loop count
    struct wavehdr_tag* lpNext;  // reserved for driver
    DWORD_PTR   reserved;        // reserved for driver
} WAVEHDR;
typedef WAVEHDR* LPWAVEHDR;

// WAVEHDR flags
#define WHDR_DONE       0x00000001
#define WHDR_PREPARED   0x00000002
#define WHDR_BEGINLOOP  0x00000004
#define WHDR_ENDLOOP    0x00000008
#define WHDR_INQUEUE    0x00000010

// Callback function pointer types
#define CALLBACK_FUNCTION  0x00030000
#define CALLBACK_NULL      0x00000000
#define CALLBACK_EVENT     0x00050000
#define CALLBACK_THREAD    0x00020000
#define CALLBACK_WAVE      0x00010000

// ─── waveOut API ────────────────────────────────────────────────
typedef HANDLE HWAVEOUT;

typedef struct {
    WORD  wMid;              // manufacturer ID
    WORD  wPid;              // product ID
    UINT  vDriverVersion;    // driver version
    char  szPname[32];       // product name
    DWORD dwFormats;         // supported formats bitmask
    WORD  wChannels;         // number of channels
    WORD  wReserved1;        // alignment
    DWORD dwSupport;         // capability flags
} WAVEOUTCAPS;
typedef WAVEOUTCAPS* LPWAVEOUTCAPS;

// dwFormats bitmask values
#define WAVE_FORMAT_1M08  0x00000001
#define WAVE_FORMAT_1S08  0x00000002
#define WAVE_FORMAT_1M16  0x00000004
#define WAVE_FORMAT_1S16  0x00000008
#define WAVE_FORMAT_2M08  0x00000010
#define WAVE_FORMAT_2S08  0x00000020
#define WAVE_FORMAT_2M16  0x00000040
#define WAVE_FORMAT_2S16  0x00000080
#define WAVE_FORMAT_4M08  0x00000100
#define WAVE_FORMAT_4S08  0x00000200
#define WAVE_FORMAT_4M16  0x00000400
#define WAVE_FORMAT_4S16  0x00000800

// waveOut functions
UINT waveOutGetNumDevs(void);
MMRESULT waveOutGetDevCapsA(UINT_PTR uDeviceID, LPWAVEOUTCAPS caps, UINT cbcaps);
MMRESULT waveOutOpen(HWAVEOUT* phwo, UINT_PTR uDeviceID,
                     LPCWAVEFORMATEX pwfx, DWORD_PTR dwCallback,
                     DWORD_PTR dwInstance, DWORD fdwOpen);
MMRESULT waveOutClose(HWAVEOUT hwo);
MMRESULT waveOutPrepareHeader(HWAVEOUT hwo, LPWAVEHDR pwh, UINT cbwh);
MMRESULT waveOutUnprepareHeader(HWAVEOUT hwo, LPWAVEHDR pwh, UINT cbwh);
MMRESULT waveOutWrite(HWAVEOUT hwo, LPWAVEHDR pwh, UINT cbwh);
MMRESULT waveOutPause(HWAVEOUT hwo);
MMRESULT waveOutRestart(HWAVEOUT hwo);
MMRESULT waveOutReset(HWAVEOUT hwo);
MMRESULT waveOutGetPosition(HWAVEOUT hwo, void* pmmt, UINT cbmmt);
MMRESULT waveOutSetVolume(HWAVEOUT hwo, DWORD dwVolume);
MMRESULT waveOutGetVolume(HWAVEOUT hwo, LPDWORD pdwVolume);
MMRESULT waveOutGetID(HWAVEOUT hwo, UINT* puDeviceID);
UINT waveOutMessage(HWAVEOUT hwo, UINT uMsg, DWORD_PTR dw1, DWORD_PTR dw2);

// waveOut messages
#define WOM_OPEN    0x3BB
#define WOM_CLOSE   0x3BC
#define WOM_DONE    0x3BD

// ─── waveIn API ─────────────────────────────────────────────────
typedef HANDLE HWAVEIN;

typedef struct {
    WORD  wMid;
    WORD  wPid;
    UINT  vDriverVersion;
    char  szPname[32];
    DWORD dwFormats;
    WORD  wChannels;
    WORD  wReserved1;
} WAVEINCAPS;
typedef WAVEINCAPS* LPWAVEINCAPS;

UINT waveInGetNumDevs(void);
MMRESULT waveInGetDevCapsA(UINT_PTR uDeviceID, LPWAVEINCAPS caps, UINT cbcaps);
MMRESULT waveInOpen(HWAVEIN* phwi, UINT_PTR uDeviceID,
                    LPCWAVEFORMATEX pwfx, DWORD_PTR dwCallback,
                    DWORD_PTR dwInstance, DWORD fdwOpen);
MMRESULT waveInClose(HWAVEIN hwi);
MMRESULT waveInPrepareHeader(HWAVEIN hwi, LPWAVEHDR pwh, UINT cbwh);
MMRESULT waveInUnprepareHeader(HWAVEIN hwi, LPWAVEHDR pwh, UINT cbwh);
MMRESULT waveInAddBuffer(HWAVEIN hwi, LPWAVEHDR pwh, UINT cbwh);
MMRESULT waveInStart(HWAVEIN hwi);
MMRESULT waveInStop(HWAVEIN hwi);
MMRESULT waveInReset(HWAVEIN hwi);
MMRESULT waveInGetID(HWAVEIN hwi, UINT* puDeviceID);

// ─── WASAPI Session API ─────────────────────────────────────────

// AUDCLNT sharing modes
#define AUDCLNT_SHAREMODE_SHARED    0
#define AUDCLNT_SHAREMODE_EXCLUSIVE 1

// AUDCLNT stream flags
#define AUDCLNT_STREAMFLAGS_AUTOSELECT     0x00080000
#define AUDCLNT_STREAMFLAGS_RATEADJUST     0x10000000
#define AUDCLNT_STREAMFLAGS_LOOPBACK       0x00020000

// AUDCLNT error codes
#define AUDCLNT_ERR_DEVICE_IN_USE          0x8889000A
#define AUDCLNT_ERR_DEVICE_INVALIDATED     0x88890004
#define AUDCLNT_ERR_UNSUPPORTED_FORMAT     0x88890008
#define AUDCLNT_ERR_BUFFER_SIZE_ERROR      0x88890019
#define AUDCLNT_E_OUT_OF_ORDER             0x88890002
#define AUDCLNT_E_BUFFER_ERROR             0x88890018
#define AUDCLNT_S_BUFFER_EMPTY             0x88890001
#define AUDCLNT_S_COMPLETE                 0x00000001
#define AUDCLNT_S_POSITION_STALLED         0x88890006

// WASAPI handles
typedef HANDLE IAudioClient;
typedef HANDLE IAudioRenderClient;
typedef HANDLE IAudioCaptureClient;
typedef HANDLE IUnknown;

// WAVEFORMATEXTENSIBLE
typedef struct {
    WAVEFORMATEX  Format;
    DWORD         dwChannelMask;
    GUID          SubFormat;
} WAVEFORMATEXTENSIBLE;

// IAudioClient operations
MMRESULT audc_client_initialize(IAudioClient* client, UINT shareMode,
                                DWORD streamFlags, UINT64 hnsBufferDuration,
                                UINT64 hnsPeriodicity,
                                LPCWAVEFORMATEX pFormat,
                                LPCWAVEFORMATEX* ppMixFormat);
MMRESULT audc_client_get_buffer_size(IAudioClient* client, UINT* pNumBufferFrames);
MMRESULT audc_client_get_period(IAudioClient* client, UINT* pDefaultDevicePeriod,
                                UINT* pMinimumDevicePeriod);
MMRESULT audc_client_start(IAudioClient* client);
MMRESULT audc_client_stop(IAudioClient* client);
MMRESULT audc_client_reset(IAudioClient* client);
MMRESULT audc_client_get_state(IAudioClient* client, DWORD* pState);
MMRESULT audc_client_get_current_padding(IAudioClient* client, UINT* pNumPaddingFrames);

// IAudioRenderClient operations
MMRESULT audrc_get_buffer(IAudioRenderClient* rc, UINT NumFramesRequested,
                          BYTE** ppData);
MMRESULT audrc_release_buffer(IAudioRenderClient* rc, UINT NumFramesWritten,
                              DWORD dwFlags);

// IAudioCaptureClient operations
MMRESULT audcc_get_buffer(IAudioCaptureClient* cc, BYTE** ppData,
                          UINT* pNumFramesToRead, DWORD* pdwFlags,
                          UINT64* pqwDevicePosition, UINT64* pqwAudioTimestamp);
MMRESULT audcc_release_buffer(IAudioCaptureClient* cc, UINT NumFramesRead);

// WASAPI factory
MMRESULT audc_factory_activate(LPCSTR iid, void** ppInterface);
MMRESULT audc_release(IUnknown* iface);

// ─── Mixer API (volume control per line) ────────────────────────
typedef HANDLE HMIXER;

#define MIXERLINE_LINEF_ACTIVE    0x00000001
#define MIXERLINE_LINEF_SOURCE    0x80000000
#define MIXERLINE_TYPE_AUDIO      0

#define MIXERCONTROL_CONTROLTYPE_VOLUME  0x50030001
#define MIXERCONTROL_CONTROLTYPE_MUTE    0x50030002
#define MIXERCONTROL_CONTROLTYPE_BOOLEAN 0x50030000
#define MIXERCONTROL_CONTROLTYPE_FADER   0x50030005
#define MIXERCONTROL_CONTROLTYPE_SLIDER  0x50030004

typedef struct {
    DWORD cbStruct;
    DWORD dwDestination;
    DWORD dwSource;
    DWORD dwLineID;
    DWORD fdwLine;
    DWORD dwUser;
    DWORD dwComponentType;
    DWORD cChannels;
    DWORD cConnections;
    DWORD cControls;
    char  szShortName[16];
    char  szName[64];
    DWORD dwType;
    DWORD dwDeviceID;
    WORD  wMid;
    WORD  wPid;
    DWORD vDriverVersion;
} MIXERLINE;
typedef MIXERLINE* LPMIXERLINE;

typedef struct {
    DWORD cbStruct;
    DWORD dwControlID;
    DWORD dwControlType;
    DWORD cChannels;
    DWORD cMultipleItems;
    DWORD fdwControl;
    DWORD cSteps;
    DWORD dwMinimum;
    DWORD dwMaximum;
    void* reserved;
    char  szShortName[16];
    char  szName[64];
} MIXERCONTROL;
typedef MIXERCONTROL* LPMIXERCONTROL;

typedef struct {
    DWORD cbStruct;
    DWORD dwLineID;
    DWORD dwControlType;
    DWORD cControls;
    DWORD cbmxctrl;
    MIXERCONTROL* pamxctrl;
    char  szName[64];
} MIXERLINECONTROLS;
typedef MIXERLINECONTROLS* LPMIXERLINECONTROLS;

typedef struct {
    DWORD cbStruct;
    DWORD dwControlID;
    DWORD cChannels;
    DWORD hwndOwner;
    DWORD cMultipleItems;
    DWORD dwLineID;
    DWORD dwValue1;
    DWORD dwValue2;
} MIXERCONTROLDETAILS;
typedef MIXERCONTROLDETAILS* LPMIXERCONTROLDETAILS;

typedef struct {
    DWORD dwValue;
} MIXERCONTROLDETAILS_UNSIGNED;
typedef MIXERCONTROLDETAILS_UNSIGNED* LPMIXERCONTROLDETAILS_UNSIGNED;

MMRESULT mixerOpen(HMIXER* phmx, UINT uMxId, DWORD_PTR dwCallback,
                   DWORD_PTR dwInstance, DWORD fdwOpen);
MMRESULT mixerClose(HMIXER hmx);
UINT     mixerGetNumDevs(void);
MMRESULT mixerGetLineInfoA(HMIXER hmxobj, LPMIXERLINE pmxl, DWORD fdwInfo);
MMRESULT mixerGetLineControlsA(HMIXER hmxobj, LPMIXERLINECONTROLS pmxlc,
                               DWORD fdwControls);
MMRESULT mixerSetControlDetails(HMIXER hmxobj, LPMIXERCONTROLDETAILS pmxcd,
                                DWORD fdwDetails);
MMRESULT mixerGetControlDetailsA(HMIXER hmxobj, LPMIXERCONTROLDETAILS pmxcd,
                                 DWORD fdwDetails);

// ─── Multimedia timer ───────────────────────────────────────────
DWORD timeGetTime(void);
MMRESULT timeBeginPeriod(UINT uPeriod);
MMRESULT timeEndPeriod(UINT uPeriod);

#endif /* WASAPI_H */
