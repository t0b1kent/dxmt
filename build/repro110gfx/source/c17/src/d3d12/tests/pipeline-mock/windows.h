#pragma once
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
using DWORD = uint32_t; using UINT = uint32_t; using BOOL = int;
using HRESULT = int32_t; using HANDLE = void *; using WCHAR = wchar_t;
struct LARGE_INTEGER { int64_t QuadPart; };
struct FILETIME { uint32_t dwLowDateTime, dwHighDateTime; };
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INVALID_FILE_ATTRIBUTES UINT32_MAX
constexpr DWORD GENERIC_READ=1, GENERIC_WRITE=2, FILE_SHARE_READ=4, CREATE_NEW=1;
constexpr DWORD FILE_ATTRIBUTE_NORMAL=0, FILE_ATTRIBUTE_DIRECTORY=16, PAGE_READWRITE=4, FILE_MAP_WRITE=2;
namespace mock {
inline std::atomic<uint64_t> ticks{100000000}, calls{0};
inline std::atomic<bool> arm{false}, fail_clock{false};
inline bool fail_create=false;
inline void *mapped=nullptr;
inline size_t bytes=0;
inline thread_local DWORD error=123;
inline DWORD Env(const char *name, char *value, DWORD size) {
  calls++; error=999;
  const char *v=std::getenv(name); if(!v) return 0;
  DWORD n=DWORD(std::strlen(v)); if(n>=size) return n+1;
  std::memcpy(value,v,n+1); return n;
}
}
inline DWORD GetLastError(){return mock::error;}
inline void SetLastError(DWORD e){mock::error=e;}
inline DWORD GetEnvironmentVariableA(const char *n,char *v,DWORD s){return mock::Env(n,v,s);}
inline DWORD GetEnvironmentVariableW(const wchar_t *n,wchar_t *v,DWORD s){
  std::string name; while(*n) name+=char(*n++);
  char temp[4096]; DWORD size=s<4096?s:4096;
  DWORD got=mock::Env(name.c_str(),temp,size);
  if(got && got<size) for(DWORD i=0;i<=got;i++) v[i]=wchar_t(temp[i]);
  return got;
}
inline BOOL QueryPerformanceFrequency(LARGE_INTEGER *v){mock::calls++;mock::error=999;v->QuadPart=1000000;return 1;}
inline BOOL QueryPerformanceCounter(LARGE_INTEGER *v){mock::calls++;mock::error=999;v->QuadPart=mock::ticks.fetch_add(1);return !mock::fail_clock;}
inline void GetSystemTimeAsFileTime(FILETIME *v){mock::error=999;v->dwHighDateTime=1;v->dwLowDateTime=0;}
inline DWORD GetCurrentProcessId(){mock::error=999;return 42;}
inline DWORD GetCurrentThreadId(){static std::atomic<DWORD> next{1};thread_local DWORD id=next++;mock::error=999;return id;}
inline HANDLE CreateFileW(const WCHAR*,DWORD,DWORD,void*,DWORD,DWORD,void*){mock::error=999;return mock::fail_create?INVALID_HANDLE_VALUE:(HANDLE)1;}
inline HANDLE CreateFileMappingW(HANDLE,void*,DWORD,DWORD,DWORD bytes,void*){mock::bytes=bytes;return (HANDLE)2;}
inline void *MapViewOfFile(HANDLE,DWORD,DWORD,DWORD,size_t size){mock::mapped=std::calloc(1,size);return mock::mapped;}
inline BOOL UnmapViewOfFile(void *p){std::free(p);mock::mapped=nullptr;mock::error=999;return 1;}
inline BOOL FlushViewOfFile(void*,size_t){mock::error=999;return 1;}
inline BOOL CloseHandle(HANDLE){mock::error=999;return 1;}
inline DWORD GetFileAttributesW(const WCHAR*){mock::error=999;return mock::arm?FILE_ATTRIBUTE_NORMAL:INVALID_FILE_ATTRIBUTES;}
