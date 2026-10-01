// SPDX-License-Identifier: GPL-3.0-or-later
// Narrow offline BakkesMod loader for Proton's module/platform detection issue.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <winternl.h>
#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

std::wstring lower(std::wstring value) {
  std::transform(value.begin(),value.end(),value.begin(),[](wchar_t c){return std::towlower(c);});return value;
}
struct Handle {
  HANDLE value{INVALID_HANDLE_VALUE};
  ~Handle(){if(value && value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
};
std::wstring command_line(HANDLE process) {
  using Query=NTSTATUS(NTAPI*)(HANDLE,PROCESSINFOCLASS,PVOID,ULONG,PULONG);
  auto query=reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQueryInformationProcess"));
  if(!query)return {};
  ULONG needed{};query(process,static_cast<PROCESSINFOCLASS>(60),nullptr,0,&needed);
  if(needed && needed<=131072) {
    std::vector<std::byte> buffer(needed);
    if(query(process,static_cast<PROCESSINFOCLASS>(60),buffer.data(),needed,&needed)>=0) {
      auto* text=reinterpret_cast<UNICODE_STRING*>(buffer.data());
      const auto start=reinterpret_cast<std::uintptr_t>(buffer.data());
      const auto ptr=reinterpret_cast<std::uintptr_t>(text->Buffer);
      if(ptr>=start && ptr+text->Length<=start+buffer.size())return {text->Buffer,text->Length/sizeof(wchar_t)};
    }
  }
  // Wine does not implement ProcessCommandLineInformation in every build.
  // Read the actual target's PEB instead; never infer offline mode from a name.
  PROCESS_BASIC_INFORMATION info{};PEB peb{};RTL_USER_PROCESS_PARAMETERS parameters{};
  if(query(process,ProcessBasicInformation,&info,sizeof(info),&needed)<0 ||
     !ReadProcessMemory(process,info.PebBaseAddress,&peb,sizeof(peb),nullptr) ||
     !ReadProcessMemory(process,peb.ProcessParameters,&parameters,sizeof(parameters),nullptr) ||
     !parameters.CommandLine.Length || parameters.CommandLine.Length>32768)return {};
  std::wstring text(parameters.CommandLine.Length/sizeof(wchar_t),L'\0');
  if(!ReadProcessMemory(process,parameters.CommandLine.Buffer,text.data(),parameters.CommandLine.Length,nullptr))return {};
  return text;
}
int wmain() {
  wchar_t appdata[32768]{};
  if(!GetEnvironmentVariableW(L"APPDATA",appdata,32768))return 1;
  const auto folder=std::filesystem::path(appdata)/L"bakkesmod/bakkesmod";
  std::ofstream log(folder/L"bridge-loader.log",std::ios::trunc);
  auto* old_error=std::cerr.rdbuf(log.rdbuf());auto* old_output=std::cout.rdbuf(log.rdbuf());
  struct RestoreStreams {std::streambuf* error;std::streambuf* output;~RestoreStreams(){std::cerr.rdbuf(error);std::cout.rdbuf(output);}} restore{old_error,old_output};
  std::cout<<"Offline bridge loader started\n";log.flush();
  Handle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0)};
  PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);DWORD pid{},parent{};
  std::wstring parent_name;
  if(!Process32FirstW(snapshot.value,&entry)){std::cerr<<"Cannot enumerate processes\n";return 1;}
  do{if(lower(entry.szExeFile)==L"rocketleague.exe"){if(pid){std::cerr<<"Multiple RL copies; refusing\n";return 1;}pid=entry.th32ProcessID;parent=entry.th32ParentProcessID;}}while(Process32NextW(snapshot.value,&entry));
  if(!pid){std::cerr<<"Offline RocketLeague.exe is not running\n";return 1;}
  Process32FirstW(snapshot.value,&entry);
  do{if(entry.th32ProcessID==parent)parent_name=lower(entry.szExeFile);}while(Process32NextW(snapshot.value,&entry));
  if(parent_name.empty()||parent_name.find(L"easyanticheat")!=std::wstring::npos||parent_name.find(L"_eac.exe")!=std::wstring::npos){std::cerr<<"Unverified or EAC parent; refusing\n";return 1;}
  Handle process{OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ|PROCESS_VM_WRITE|PROCESS_VM_OPERATION|PROCESS_CREATE_THREAD,FALSE,pid)};
  if(!process.value){std::cerr<<"Cannot open RL process: "<<GetLastError()<<'\n';return 1;}
  if(lower(command_line(process.value)).find(L"-noeac")==std::wstring::npos){std::cerr<<"RL command line does not explicitly select -NoEAC; refusing\n";return 1;}
  const auto path=(std::filesystem::path(appdata)/L"bakkesmod/bakkesmod/dll/bakkesmod.dll").wstring();
  if(!std::filesystem::is_regular_file(path)){std::cerr<<"Official installed BakkesMod DLL missing\n";return 1;}
  const auto size=(path.size()+1)*sizeof(wchar_t);
  auto* remote=VirtualAllocEx(process.value,nullptr,size,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
  if(!remote||!WriteProcessMemory(process.value,remote,path.c_str(),size,nullptr)){std::cerr<<"Cannot copy DLL path\n";return 1;}
  auto load=reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"LoadLibraryW"));
  Handle thread{CreateRemoteThread(process.value,nullptr,0,load,remote,0,nullptr)};
  if(!thread.value){VirtualFreeEx(process.value,remote,0,MEM_RELEASE);std::cerr<<"Cannot start loader: "<<GetLastError()<<'\n';return 1;}
  if(WaitForSingleObject(thread.value,10000)!=WAIT_OBJECT_0){std::cerr<<"Loader still pending; no thread terminated\n";return 1;}
  DWORD result{};GetExitCodeThread(thread.value,&result);VirtualFreeEx(process.value,remote,0,MEM_RELEASE);
  if(!result){std::cerr<<"LoadLibraryW failed; check game/runtime dependencies\n";return 1;}
  std::cout<<"Offline BakkesMod LoadLibraryW completed; plugin log still required for confirmation\n";return 0;
}
