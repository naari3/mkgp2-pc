/*
 * Camera devices on Windows, through Media Foundation's source reader.
 *
 * The device back end of camera.c: open, wait for one picture, close, all
 * on camera.c's capture thread. The reader is asked for 32-bit RGB whatever
 * the device delivers (its video processing converts), at the size the
 * device starts with; camera.c cuts and shrinks that to 320x240.
 *
 * Media Foundation is reached through LoadLibrary, not through the import
 * table, and the few identifiers needed are spelled out below: the game
 * must still start on a Windows without the media components (the "N"
 * editions), and the link lines stay as they are.
 *
 * Which device: MKGP2PC_CAMERA, read here as wide text so that device names
 * outside the system code page still compare ("default" or nothing: the
 * first; a number: that one, counted from 1; otherwise the first whose name
 * contains the text, letter case ignored). MKGP2PC_TRACE_CAMERA=1 lists the
 * devices found.
 *
 * State 2026-11-12: compiled and linked with MinGW only. Not run: no camera
 * and no Windows where this was written.
 */
#ifdef _WIN32
#define COBJMACROS
#include <windows.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include <mkgp2pc/camera.h>

/* Values from the Windows SDK headers (mfapi.h, mfidl.h, mfobjects.h, mfreadwrite.h). */
static const GUID kSourceType = {0xc60ac5fe, 0x252a, 0x478f, {0xa0, 0xef, 0xbc, 0x8f, 0xa5, 0xf7, 0xca, 0xd3}};
static const GUID kSourceTypeVideo = {0x8ac3587a, 0x4ae7, 0x42d8, {0x99, 0xe0, 0x0a, 0x60, 0x13, 0xee, 0xf9, 0x0f}};
static const GUID kFriendlyName = {0x60d0e559, 0x52f8, 0x4fa2, {0xbb, 0xce, 0xac, 0xdb, 0x34, 0xa8, 0xec, 0x01}};
static const GUID kReaderVideoProcessing = {0xfb394f3d, 0xccf1, 0x42ee, {0xbb, 0xb3, 0xf9, 0xb8, 0x45, 0xd5, 0x68, 0x1d}};
static const GUID kMajorType = {0x48eba18e, 0xf8c9, 0x4687, {0xbf, 0x11, 0x0a, 0x74, 0xc9, 0xf9, 0x6a, 0x8f}};
static const GUID kSubtype = {0xf7e34c9a, 0x42e8, 0x4714, {0xb7, 0x4b, 0xcb, 0x29, 0xd7, 0x2c, 0x35, 0xe5}};
static const GUID kFrameSize = {0x1652c33d, 0xd6b2, 0x4012, {0xb8, 0x34, 0x72, 0x03, 0x08, 0x49, 0xa3, 0x7d}};
static const GUID kDefaultStride = {0x644b4e48, 0x1e02, 0x4516, {0xb0, 0xeb, 0xc0, 0x1c, 0xa9, 0xd4, 0x9a, 0xc6}};
static const GUID kTypeVideo = {0x73646976, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID kFormatRgb32 = {22, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID kIidMediaSource = {0x279a808d, 0xaec7, 0x40c8, {0x9c, 0x6b, 0xa6, 0xb4, 0x92, 0xc7, 0x8a, 0x66}};
static const GUID kIid2DBuffer = {0x7dc9d5f9, 0x9ed9, 0x44ec, {0x9b, 0xbf, 0x06, 0x00, 0xbb, 0x58, 0x9f, 0xbb}};

#define kFirstVideoStream ((DWORD)0xFFFFFFFC)
enum { kMfVersion = 0x00020070, kMfStartupLite = 1 };

static struct {
  int loaded; /* 1 yes, -1 not there */
  HRESULT(WINAPI* CoInitializeEx)(LPVOID, DWORD);
  void(WINAPI* CoUninitialize)(void);
  void(WINAPI* CoTaskMemFree)(LPVOID);
  HRESULT(WINAPI* MFStartup)(ULONG, DWORD);
  HRESULT(WINAPI* MFShutdown)(void);
  HRESULT(WINAPI* MFCreateAttributes)(IMFAttributes**, UINT32);
  HRESULT(WINAPI* MFCreateMediaType)(IMFMediaType**);
  HRESULT(WINAPI* MFEnumDeviceSources)(IMFAttributes*, IMFActivate***, UINT32*);
  HRESULT(WINAPI* MFCreateSourceReaderFromMediaSource)(IMFMediaSource*, IMFAttributes*, IMFSourceReader**);
} mf;

static int load(void) {
  HMODULE ole, plat, mfdll, rw;
  if (mf.loaded != 0) {
    return mf.loaded > 0;
  }
  mf.loaded = -1;
  ole = LoadLibraryW(L"ole32.dll");
  plat = LoadLibraryW(L"mfplat.dll");
  mfdll = LoadLibraryW(L"mf.dll");
  rw = LoadLibraryW(L"mfreadwrite.dll");
  if (ole == NULL || plat == NULL || mfdll == NULL || rw == NULL) {
    return 0;
  }
#define GET(lib, name) (mf.name = (void*)GetProcAddress(lib, #name)) != NULL
  if (GET(ole, CoInitializeEx) && GET(ole, CoUninitialize) && GET(ole, CoTaskMemFree) && GET(plat, MFStartup) &&
      GET(plat, MFShutdown) && GET(plat, MFCreateAttributes) && GET(plat, MFCreateMediaType) &&
      GET(mfdll, MFEnumDeviceSources) && GET(rw, MFCreateSourceReaderFromMediaSource)) {
    mf.loaded = 1;
  }
#undef GET
  return mf.loaded > 0;
}

static IMFMediaSource* s_source;
static IMFSourceReader* s_reader;
static int s_width, s_height, s_stride;
static int s_com, s_started;

static int tracing(void) {
  const char* e = getenv("MKGP2PC_TRACE_CAMERA");
  return e != NULL && e[0] != 0 && e[0] != '0';
}

static void lower(wchar_t* s) { CharLowerW(s); }

/* Size and row distance of what the reader hands out now. */
static int read_format(void) {
  IMFMediaType* type = NULL;
  UINT64 size = 0;
  UINT32 stride = 0;
  if (FAILED(IMFSourceReader_GetCurrentMediaType(s_reader, kFirstVideoStream, &type))) {
    return 0;
  }
  IMFMediaType_GetUINT64(type, &kFrameSize, &size);
  s_width = (int)(size >> 32);
  s_height = (int)(size & 0xFFFFFFFFu);
  /* without the attribute: rows from the top, as the source reader's
   * converter delivers them */
  s_stride = SUCCEEDED(IMFMediaType_GetUINT32(type, &kDefaultStride, &stride)) && stride != 0 ? (int)(INT32)stride : s_width * 4;
  IMFMediaType_Release(type);
  return s_width > 0 && s_height > 0;
}

void mkgp2pc_camera_device_close(void) {
  if (s_reader != NULL) {
    IMFSourceReader_Release(s_reader);
    s_reader = NULL;
  }
  if (s_source != NULL) {
    IMFMediaSource_Shutdown(s_source);
    IMFMediaSource_Release(s_source);
    s_source = NULL;
  }
  if (s_started) {
    mf.MFShutdown();
    s_started = 0;
  }
  if (s_com) {
    mf.CoUninitialize();
    s_com = 0;
  }
}

static int failed(char* error, int errorSize, const char* what, HRESULT hr) {
  snprintf(error, (size_t)errorSize, "%s (0x%08lX)", what, (unsigned long)hr);
  mkgp2pc_camera_device_close();
  return -1;
}

int mkgp2pc_camera_device_open(const char* name, char* error, int errorSize) {
  wchar_t want[256] = L"";
  IMFAttributes* attributes = NULL;
  IMFActivate** devices = NULL;
  IMFMediaType* type = NULL;
  UINT32 count = 0, pick = 0;
  int found = 0, number = 0;
  HRESULT hr;
  (void)name;
  if (!load()) {
    snprintf(error, (size_t)errorSize, "Media Foundation is not installed on this Windows");
    return -1;
  }
  GetEnvironmentVariableW(L"MKGP2PC_CAMERA", want, 256);
  want[255] = 0;
  lower(want);
  if (wcscmp(want, L"default") == 0) {
    want[0] = 0;
  }
  if (want[0] != 0 && wcsspn(want, L"0123456789") == wcslen(want)) {
    number = _wtoi(want);
  }

  hr = mf.CoInitializeEx(NULL, COINIT_MULTITHREADED);
  s_com = SUCCEEDED(hr);
  hr = mf.MFStartup(kMfVersion, kMfStartupLite);
  if (FAILED(hr)) {
    return failed(error, errorSize, "Media Foundation does not start", hr);
  }
  s_started = 1;

  hr = mf.MFCreateAttributes(&attributes, 1);
  if (SUCCEEDED(hr)) {
    hr = IMFAttributes_SetGUID(attributes, &kSourceType, &kSourceTypeVideo);
  }
  if (SUCCEEDED(hr)) {
    hr = mf.MFEnumDeviceSources(attributes, &devices, &count);
  }
  if (attributes != NULL) {
    IMFAttributes_Release(attributes);
  }
  if (FAILED(hr)) {
    return failed(error, errorSize, "the cameras cannot be listed", hr);
  }
  for (UINT32 i = 0; i < count; i++) {
    wchar_t* text = NULL;
    UINT32 length = 0;
    if (SUCCEEDED(IMFActivate_GetAllocatedString(devices[i], &kFriendlyName, &text, &length)) && text != NULL) {
      if (tracing()) {
        fprintf(stderr, "camera: device %u: %ls\n", (unsigned)i + 1, text);
      }
      lower(text);
      if (!found && number == 0 && (want[0] == 0 || wcsstr(text, want) != NULL)) {
        found = 1;
        pick = i;
      }
      mf.CoTaskMemFree(text);
    }
  }
  if (number >= 1 && (UINT32)number <= count) {
    found = 1;
    pick = (UINT32)number - 1;
  }
  if (!found && want[0] == 0 && count > 0) { /* a device without a name */
    found = 1;
  }
  hr = found ? IMFActivate_ActivateObject(devices[pick], &kIidMediaSource, (void**)&s_source) : S_OK;
  for (UINT32 i = 0; i < count; i++) {
    IMFActivate_Release(devices[i]);
  }
  mf.CoTaskMemFree(devices);
  if (!found) {
    snprintf(error, (size_t)errorSize, count == 0 ? "no camera is connected" : "none of the %u cameras matches MKGP2PC_CAMERA", (unsigned)count);
    mkgp2pc_camera_device_close();
    return -1;
  }
  if (FAILED(hr)) {
    return failed(error, errorSize, "the camera cannot be opened (in use by another program?)", hr);
  }

  attributes = NULL;
  hr = mf.MFCreateAttributes(&attributes, 1);
  if (SUCCEEDED(hr)) {
    hr = IMFAttributes_SetUINT32(attributes, &kReaderVideoProcessing, TRUE);
  }
  if (SUCCEEDED(hr)) {
    hr = mf.MFCreateSourceReaderFromMediaSource(s_source, attributes, &s_reader);
  }
  if (attributes != NULL) {
    IMFAttributes_Release(attributes);
  }
  if (FAILED(hr)) {
    return failed(error, errorSize, "no reader for the camera", hr);
  }
  hr = mf.MFCreateMediaType(&type);
  if (SUCCEEDED(hr)) {
    hr = IMFMediaType_SetGUID(type, &kMajorType, &kTypeVideo);
  }
  if (SUCCEEDED(hr)) {
    hr = IMFMediaType_SetGUID(type, &kSubtype, &kFormatRgb32);
  }
  if (SUCCEEDED(hr)) {
    hr = IMFSourceReader_SetCurrentMediaType(s_reader, kFirstVideoStream, NULL, type);
  }
  if (type != NULL) {
    IMFMediaType_Release(type);
  }
  if (FAILED(hr)) {
    return failed(error, errorSize, "the camera's pictures cannot be converted to RGB", hr);
  }
  if (!read_format()) {
    return failed(error, errorSize, "the camera does not tell its picture size", E_FAIL);
  }
  fprintf(stderr, "camera: device %u open, %dx%d\n", (unsigned)pick + 1, s_width, s_height);
  return 0;
}

int mkgp2pc_camera_device_grab(void) {
  IMFSample* sample = NULL;
  IMFMediaBuffer* buffer = NULL;
  IMF2DBuffer* rows = NULL;
  DWORD stream = 0, flags = 0;
  LONGLONG time = 0;
  HRESULT hr;
  if (s_reader == NULL) {
    return -1;
  }
  hr = IMFSourceReader_ReadSample(s_reader, kFirstVideoStream, 0, &stream, &flags, &time, &sample);
  if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM)) != 0) {
    if (sample != NULL) {
      IMFSample_Release(sample);
    }
    return -1;
  }
  if ((flags & (MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED | MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED)) != 0) {
    read_format();
  }
  if (sample == NULL) {
    return 0; /* a gap in the stream */
  }
  if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(sample, &buffer)) && buffer != NULL) {
    BYTE* data = NULL;
    LONG pitch = 0;
    DWORD length = 0;
    if (SUCCEEDED(IMFMediaBuffer_QueryInterface(buffer, &kIid2DBuffer, (void**)&rows)) && rows != NULL) {
      /* the buffer itself says where the top row is and which way the rows go */
      if (SUCCEEDED(IMF2DBuffer_Lock2D(rows, &data, &pitch))) {
        mkgp2pc_camera_store(data, s_width, s_height, (int)pitch, 4);
        IMF2DBuffer_Unlock2D(rows);
      }
      IMF2DBuffer_Release(rows);
    } else if (SUCCEEDED(IMFMediaBuffer_Lock(buffer, &data, NULL, &length))) {
      int stride = s_stride, size = (stride < 0 ? -stride : stride) * s_height;
      if (size > 0 && length >= (DWORD)size) {
        mkgp2pc_camera_store(stride < 0 ? data + (size_t)(-stride) * (size_t)(s_height - 1) : data, s_width, s_height, stride, 4);
      }
      IMFMediaBuffer_Unlock(buffer);
    }
    IMFMediaBuffer_Release(buffer);
  }
  IMFSample_Release(sample);
  return 0;
}
#else
typedef int mkgp2pc_camera_mf_not_built; /* Windows only */
#endif
