#include <Windows.h>
#include <iostream>
#include <vector>
#include <codecvt>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <fstream>
#include <TlHelp32.h>
#include <winternl.h>

#pragma comment(lib, "Shlwapi.lib")
#include <Shlwapi.h>

#include "ui.h"
#include "settings.h"
#include "rfu.h"
#include "procutil.h"
#include "sigscan.h"
#include "nlohmann.hpp"

#define ROBLOX_BASIC_ACCESS (PROCESS_QUERY_INFORMATION | PROCESS_VM_READ)
#define	ROBLOX_WRITE_ACCESS (PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE)

HANDLE SingletonMutex;

enum class RobloxHandleType
{
	None,
	Client,
	UWP,
	Studio
};

struct RobloxProcessHandle
{
	DWORD id;
	HANDLE handle;
	RobloxHandleType type;
	bool can_write;

	RobloxProcessHandle(DWORD process_id = 0, RobloxHandleType type = RobloxHandleType::None, bool open = false) : id(process_id), handle(NULL), type(type), can_write(false)
	{
		if (open) Open();
	};

	RobloxProcessHandle(const RobloxProcessHandle &) = delete;
	RobloxProcessHandle &operator=(const RobloxProcessHandle &) = delete;

	RobloxProcessHandle(RobloxProcessHandle &&other) noexcept
	{
		std::swap(id, other.id);
		std::swap(handle, other.handle);
		std::swap(type, other.type);
		std::swap(can_write, other.can_write);
	}

	RobloxProcessHandle &operator=(RobloxProcessHandle &&other) noexcept
	{
		if (this != &other)
		{
			if (handle) CloseHandle(handle);
			id = std::exchange(other.id, {});
			handle = std::exchange(other.handle, {});
			type = std::exchange(other.type, {});
			can_write = std::exchange(other.can_write, {});
		}
		return *this;
	}

	~RobloxProcessHandle()
	{
		if (handle)
		{
			//printf("[%p] Closing handle with type=%u, can_write=%u\n", handle, type, can_write);
			CloseHandle(handle);
		}
	}

	bool IsValid() const
	{
		return id != 0;
	}

	bool IsOpen() const
	{
		return handle != NULL;
	}

	bool Open()
	{
		can_write = type == RobloxHandleType::Studio;
		handle = OpenProcess(can_write ? ROBLOX_WRITE_ACCESS : ROBLOX_BASIC_ACCESS, FALSE, id);
		return handle != NULL;
	}

	HANDLE CreateWriteHandle() const
	{
		HANDLE new_handle = NULL;
		DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &new_handle, ROBLOX_WRITE_ACCESS, FALSE, NULL);
		return new_handle;
	}

	bool UpgradeHandle() noexcept
	{
		if (can_write) return true;
		HANDLE new_handle = CreateWriteHandle();
		if (!new_handle) return false;
		CloseHandle(handle);
		handle = new_handle;
		can_write = true;
		return true;
	}

	template <typename T>
	void Write(const void *location, const T &value) const
	{
		if (can_write)
		{
			printf("[%p] Writing to %p\n", handle, location);
			ProcUtil::Write<T>(handle, location, value);
		}
		else
		{
			auto write_handle = CreateWriteHandle();
			if (!write_handle) throw ProcUtil::WindowsException("failed to create write handle");
			printf("[%p] Writing to %p with handle %p\n", handle, location, write_handle);
			ProcUtil::Write<T>(write_handle, location, value);
			CloseHandle(write_handle);
		}
	}
};

std::vector<RobloxProcessHandle> GetRobloxProcesses(bool open_all = true, bool include_client = true, bool include_studio = true)
{
	std::vector<RobloxProcessHandle> result;
	if (include_client)
	{
		for (auto pid : ProcUtil::GetProcessIdsByImageName("RobloxPlayerBeta.exe")) result.emplace_back(pid, RobloxHandleType::Client, open_all);
		for (auto pid : ProcUtil::GetProcessIdsByImageName("Windows10Universal.exe")) result.emplace_back(pid, RobloxHandleType::UWP, open_all);
	}
	if (include_studio)
	{
		for (auto pid : ProcUtil::GetProcessIdsByImageName("RobloxStudioBeta.exe")) result.emplace_back(pid, RobloxHandleType::Studio, open_all);
	}
	return result;
}

RobloxProcessHandle GetRobloxProcess()
{
	auto processes = GetRobloxProcesses();

	if (processes.empty())
		return {};

	if (processes.size() == 1)
		return std::move(processes[0]);

	printf("Multiple processes found! Select a process to inject into (%u - %zu):\n", 1, processes.size());
	for (int i = 0; i < processes.size(); i++)
	{
		try
		{
			ProcUtil::ProcessInfo info(processes[i].handle, true);
			printf("[%d] [%s] %s\n", i + 1, info.name.c_str(), info.window_title.c_str());
		}
		catch (ProcUtil::WindowsException& e)
		{
			printf("[%d] Invalid process %p (%s, %X)\n", i + 1, processes[i].handle, e.what(), e.GetLastError());
		}
	}

	int selection;

	while (true)
	{
		printf("\n>");
		std::cin >> selection;

		if (std::cin.fail())
		{
			std::cin.clear();
			std::cin.ignore(std::cin.rdbuf()->in_avail());
			printf("Invalid input, try again\n");
			continue;
		}

		if (selection < 1 || selection > processes.size())
		{
			printf("Please enter a number between %u and %zu\n", 1, processes.size());
			continue;
		}

		break;
	}

	return std::move(processes[selection - 1]);
}

void NotifyError(const char* title, const char* error)
{
	if (Settings::SilentErrors || Settings::NonBlockingErrors)
	{
		// lol
		HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
		CONSOLE_SCREEN_BUFFER_INFO info{};
		GetConsoleScreenBufferInfo(console, &info);

		WORD color = (info.wAttributes & 0xFF00) | FOREGROUND_RED | FOREGROUND_INTENSITY;
		SetConsoleTextAttribute(console, color);

		printf("[ERROR] %s\n", error);

		SetConsoleTextAttribute(console, info.wAttributes);

		if (!Settings::SilentErrors)
		{
			UI::SetConsoleVisible(true);
		}
	}
	else
	{
		MessageBoxA(UI::Window, error, title, MB_OK);
	}
}

class RobloxProcess
{
	RobloxProcessHandle process{};
	ProcUtil::ModuleInfo main_module{};
	std::vector<const void *> ts_ptr_candidates; // task scheduler pointer candidates
	const void *fd_ptr = nullptr; // frame delay pointer
	bool use_flags_file = false;
	int retries_left = 0;

	bool BlockingLoadModuleInfo()
	{
		int tries = 5;
		int wait_time = 100;

		printf("[%p] Finding process base...\n", process.handle);

		while (true)
		{
			ProcUtil::ProcessInfo info = ProcUtil::ProcessInfo(process.handle);

			if (info.module.base != nullptr)
			{
				main_module = info.module;
				return true;
			}

			if (tries--)
			{
				printf("[%p] Retrying in %dms...\n", process.handle, wait_time);
				Sleep(wait_time);
				wait_time *= 2;
			} else
			{
				return false;
			}
		}
	}

	bool IsLikelyAntiCheatProtected() const
	{
		return process.type != RobloxHandleType::Studio && ProcUtil::IsProcess64Bit(process.handle);
	}

	std::filesystem::path GetClientAppSettingsFilePath() const
	{
		return main_module.path.parent_path() / "ClientSettings" / "ClientAppSettings.json";
	}

	std::optional<int> FetchTargetFpsDiskValue(nlohmann::json *object_out = nullptr) const
	{
		std::ifstream file(GetClientAppSettingsFilePath());

		if (file.is_open())
		{
			nlohmann::json object = nlohmann::json::parse(file, nullptr, false);
			if (!object.is_discarded())
			{
				std::optional<int> result{};

				if (object.contains("DFIntTaskSchedulerTargetFps"))
				{
					auto target_fps = object["DFIntTaskSchedulerTargetFps"];
					if (target_fps.is_number_integer())
					{
						result = target_fps.get<int>();
					}
				}

				if (object_out)
					*object_out = std::move(object);

				return result;
			}
		}

		return std::nullopt;
	}

	bool IsTargetFpsFlagActive() const
	{
		auto value = FetchTargetFpsDiskValue();
		return value.has_value() && *value > 0;
	}

	void WriteFlagsFile(int cap)
	{
		// todo: add support for registry read

		if (cap == 0) cap = 5588562;

		auto settings_file_path = GetClientAppSettingsFilePath();
		printf("[%p] Updating DFIntTaskSchedulerTargetFps in %ls to %d\n", process.handle, settings_file_path.c_str(), cap);

		nlohmann::json object{};

		// read
		auto current_cap = FetchTargetFpsDiskValue(&object);
		if ((current_cap.has_value() && *current_cap == cap) || (!current_cap.has_value() && cap < 0))
		{
			return;
		}

		// update
		object["DFIntTaskSchedulerTargetFps"] = cap;

		// try write
		{
			std::error_code ec{};
			std::filesystem::create_directory(settings_file_path.parent_path(), ec);

			std::ofstream file(settings_file_path);
			if (!file.is_open())
			{
				NotifyError("rbxfpsunlocker Error", "Failed to write ClientAppSettings.json! If running the Windows Store version of Roblox, try running Roblox FPS Unlocker as administrator or using a different unlock method.");
				return;
			}
			file << object.dump(4);
		}

		// prompt
		char message[512]{};
		sprintf_s(message, "Set DFIntTaskSchedulerTargetFps to %d in %ls\n\nRestarting Roblox may be required for changes to take effect.", cap, settings_file_path.c_str());
		MessageBoxA(UI::Window, message, "rbxfpsunlocker", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
	}

	void SetFPSCapInMemory(double cap)
	{
		if (fd_ptr)
		{
			try
			{
				static const double min_frame_delay = 1.0 / 10000.0;
				double frame_delay = cap <= 0.0 ? min_frame_delay : 1.0 / cap;

				process.Write(fd_ptr, frame_delay);
			} catch (ProcUtil::WindowsException &e)
			{
				printf("[%p] RobloxProcess::SetFPSCapInMemory failed: %s (%d)\n", process.handle, e.what(), e.GetLastError());
			}
		}
	}

	bool FindTaskScheduler()
	{
		try
		{
			const auto handle = process.handle;
			const auto start = (const uint8_t *)main_module.base;
			const auto end = start + main_module.size;

			if (ProcUtil::IsProcess64Bit(handle))
			{
				// 40 53 48 83 EC 20 0F B6 D9 E8 ?? ?? ?? ?? 86 58 04 48 83 C4 20 5B C3
				if (auto result = (const uint8_t *)ProcUtil::ScanProcess(handle, "\x40\x53\x48\x83\xEC\x20\x0F\xB6\xD9\xE8\x00\x00\x00\x00\x86\x58\x04\x48\x83\xC4\x20\x5B\xC3", "xxxxxxxxxx????xxxxxxxxx", start, end))
				{
					auto gts_fn = result + 14 + ProcUtil::Read<int32_t>(handle, result + 10);

					printf("[%p] GetTaskScheduler (sig studio): %p\n", handle, gts_fn);

					uint8_t buffer[0x100];
					if (ProcUtil::Read(handle, gts_fn, buffer, sizeof(buffer)))
					{
						if (auto inst = sigscan::scan("\x48\x8B\x05\x00\x00\x00\x00\x48\x83\xC4\x28", "xxx????xxxx", (uintptr_t)buffer, (uintptr_t)buffer + 0x100)) // mov eax, <TaskSchedulerPtr>; mov ecx, [ebp-0Ch])
						{
							const uint8_t *remote = gts_fn + (inst - buffer);
							ts_ptr_candidates = { remote + 7 + *(int32_t *)(inst + 3) };
							return true;
						}
					}
				}
				else
				{
					// Assume Byfron
					// 
					// Thought process: Fancy new anti-cheat technology makes inspecting .text a bit more troublesome than before
					// As a result, I've opted to sig GetTaskScheduler directly instead of looking for one its callers.
					// A longer, uglier signature could be used to produce a single result here,
					// but for the sake of (hopefully) increased reliability, we'll use a simple signature that returns about 8 candidates in a loaded game.

					std::unordered_set<const void *> candidates{};
					auto i = start;
					auto stop = (std::min)(end, start + 40 * 1024 * 1024); // optim: keep search roughly within .text
					const size_t candidate_threshold = 5;

					while (i < stop)
					{
						// 48 8B 05 ?? ?? ?? ?? 48 83 C4 48 C3
						auto result = (const uint8_t *)ProcUtil::ScanProcess(handle, "\x48\x8B\x05\x00\x00\x00\x00\x48\x83\xC4\x48\xC3", "xxx????xxxxx", i, stop); // mov rax, <Rel32>; add rsp, 48h; retn
						if (!result) break;
						candidates.insert(result + 7 + ProcUtil::Read<int32_t>(handle, result + 3));
						if (candidates.size() >= candidate_threshold) break;
						i = result + 1;
					}

					printf("[%p] GetTaskScheduler (sig byfron): found %zu candidates\n", handle, candidates.size());

					if (candidates.size() != candidate_threshold)
						return false; // keep looking

					ts_ptr_candidates = std::vector<const void *>(candidates.begin(), candidates.end());
					return true;
				}
			} else
			{
				// 55 8B EC 83 E4 F8 83 EC 08 E8 ?? ?? ?? ?? 8D 0C 24
				if (auto result = (const uint8_t *)ProcUtil::ScanProcess(handle, "\x55\x8B\xEC\x83\xE4\xF8\x83\xEC\x08\xE8\xDE\xAD\xBE\xEF\x8D\x0C\x24", "xxxxxxxxxx????xxx", start, end))
				{
					auto gts_fn = result + 14 + ProcUtil::Read<int32_t>(handle, result + 10);

					printf("[%p] GetTaskScheduler (sig ltcg): %p\n", handle, gts_fn);

					uint8_t buffer[0x100];
					if (ProcUtil::Read(handle, gts_fn, buffer, sizeof(buffer)))
					{
						if (auto inst = sigscan::scan("\xA1\x00\x00\x00\x00\x8B\x4D\xF4", "x????xxx", (uintptr_t)buffer, (uintptr_t)buffer + 0x100)) // mov eax, <TaskSchedulerPtr>; mov ecx, [ebp-0Ch])
						{
							//printf("[%p] Inst: %p\n", process, gts_fn + (inst - buffer));
							ts_ptr_candidates = { (const void *)(*(uint32_t *)(inst + 1)) };
							return true;
						}
					}
				}
				// 55 8B EC 83 EC 10 56 E8 ?? ?? ?? ?? 8B F0 8D 45 F0
				else if (auto result = (const uint8_t *)ProcUtil::ScanProcess(handle, "\x55\x8B\xEC\x83\xEC\x10\x56\xE8\x00\x00\x00\x00\x8B\xF0\x8D\x45\xF0", "xxxxxxxx????xxxxx", start, end))
				{
					auto gts_fn = result + 12 + ProcUtil::Read<int32_t>(handle, result + 8);

					printf("[%p] GetTaskScheduler (sig non-ltcg): %p\n", handle, gts_fn);

					uint8_t buffer[0x100];
					if (ProcUtil::Read(handle, gts_fn, buffer, sizeof(buffer)))
					{
						if (auto inst = sigscan::scan("\xA1\x00\x00\x00\x00\x8B\x4D\xF4", "x????xxx", (uintptr_t)buffer, (uintptr_t)buffer + 0x100)) // mov eax, <TaskSchedulerPtr>; mov ecx, [ebp-0Ch])
						{
							//printf("[%p] Inst: %p\n", process, gts_fn + (inst - buffer));
							ts_ptr_candidates = { (const void *)(*(uint32_t *)(inst + 1)) };
							return true;
						}
					}
				}
				// 55 8B EC 83 E4 F8 83 EC 14 56 E8 ?? ?? ?? ?? 8D 4C 24 10
				else if (auto result = (const uint8_t *)ProcUtil::ScanProcess(handle, "\x55\x8B\xEC\x83\xE4\xF8\x83\xEC\x14\x56\xE8\x00\x00\x00\x00\x8D\x4C\x24\x10", "xxxxxxxxxxx????xxxx", start, end))
				{
					auto gts_fn = result + 15 + ProcUtil::Read<int32_t>(handle, result + 11);

					printf("[%p] GetTaskScheduler (sig uwp): %p\n", handle, gts_fn);

					uint8_t buffer[0x100];
					if (ProcUtil::Read(handle, gts_fn, buffer, sizeof(buffer)))
					{
						if (auto inst = sigscan::scan("\xA1\x00\x00\x00\x00\x8B\x4D\xF4", "x????xxx", (uintptr_t)buffer, (uintptr_t)buffer + 0x100)) // mov eax, <TaskSchedulerPtr>; mov ecx, [ebp-0Ch])
						{
							ts_ptr_candidates = { (const void *)(*(uint32_t *)(inst + 1)) };
							return true;
						}
					}
				}
			}
		}
		catch (ProcUtil::WindowsException &e)
		{
		}

		return false;
	}

	size_t FindTaskSchedulerFrameDelayOffset(const void *scheduler) const
	{
		const size_t search_offset = 0x100; // ProcUtil::IsProcess64Bit(process) ? 0x200 : 0x100;

		uint8_t buffer[0x100];
		if (!ProcUtil::Read(process.handle, (const uint8_t *)scheduler + search_offset, buffer, sizeof(buffer)))
			return -1;

		/* Find the frame delay variable inside TaskScheduler (ugly, but it should survive updates unless the variable is removed or shifted)
		   (variable was at +0x150 (32-bit) and +0x180 (studio 64-bit) as of 2/13/2020) */
		for (int i = 0; i < sizeof(buffer) - sizeof(double); i += 4)
		{
			static const double frame_delay = 1.0 / 60.0;
			double difference = *(double *)(buffer + i) - frame_delay;
			difference = difference < 0 ? -difference : difference;
			if (difference < std::numeric_limits<double>::epsilon()) return search_offset + i;
		}

		return -1;
	}

public:
	const RobloxProcessHandle &GetHandle() const
	{
		return process;
	}

	bool Attach(RobloxProcessHandle handle, int retry_count)
	{
		process = std::move(handle);
		retries_left = retry_count;

		if (!BlockingLoadModuleInfo())
		{
			NotifyError("rbxfpsunlocker Error", "Failed to get process base! Restart Roblox FPS Unlocker or, if you are on a 64-bit operating system, make sure you are using the 64-bit version of Roblox FPS Unlocker.");
			retries_left = -1;
			return false;
		}
		else
		{
			printf("[%p] Process base: %p (size %zu)\n", process.handle, main_module.base, main_module.size);

			// Small windows exist where we can attach to Roblox's security daemon while it isn't being debugged (see GetRobloxProcesses)
			// As a secondary measure, check module size (daemon is about 1MB, client is about 80MB)
			if (main_module.size < 1024 * 1024 * 10)
			{
				printf("[%p] Ignoring security daemon process\n", process.handle);
				retries_left = -1;
				return false;
			}

			OnUnlockMethodUpdate();
			Tick();

			return !ts_ptr_candidates.empty() && fd_ptr != nullptr;
		}
	}

	void Tick()
	{
		if (use_flags_file)
			return;

		if (retries_left < 0)
			return; // we tried

		if (ts_ptr_candidates.empty())
		{
			const auto start_time = std::chrono::steady_clock::now();
			FindTaskScheduler();
			
			if (ts_ptr_candidates.empty())
			{
				if (retries_left-- <= 0)
					NotifyError("rbxfpsunlocker Error", "Unable to find TaskScheduler! This is probably due to a Roblox update-- watch the github for any patches or a fix.");
				return;
			}
			else
			{
				const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count();
				printf("[%p] Found TaskScheduler candidates in %lldms\n", process.handle, elapsed);
			}
		}

		if (!ts_ptr_candidates.empty() && !fd_ptr)
		{
			try
			{
				size_t fail_count = 0;

				for (const void *ts_ptr : ts_ptr_candidates)
				{
					if (auto scheduler = (const uint8_t *)(ProcUtil::ReadPointer(process.handle, ts_ptr)))
					{
						printf("[%p] Potential task scheduler: %p\n", process.handle, scheduler);

						size_t delay_offset = FindTaskSchedulerFrameDelayOffset(scheduler);
						if (delay_offset == -1)
						{
							fail_count++;
							continue; // try next
						}

						// winner
						printf("[%p] Frame delay offset: %zu (0x%zx)\n", process.handle, delay_offset, delay_offset);
						fd_ptr = scheduler + delay_offset;

						// first write
						SetFPSCap(Settings::FPSCap);
						return;
					}
					else
					{
						printf("[%p] *ts_ptr (%p) == nullptr\n", process.handle, ts_ptr);
					}
				}

				if (fail_count > 0)
				{
					// one or more candidates had valid pointers with no frame delay variable
					if (retries_left-- <= 0)
						NotifyError("rbxfpsunlocker Error", "Variable scan failed! Make sure your framerate is at ~60.0 FPS (press Shift+F5 in-game) before using Roblox FPS Unlocker.");
				}
			}
			catch (ProcUtil::WindowsException& e)
			{
				printf("[%p] RobloxProcess::Tick failed: %s (%d)\n", process.handle, e.what(), e.GetLastError());
				if (retries_left-- <= 0)
					NotifyError("rbxfpsunlocker Error", "An exception occurred while performing the variable scan.");
			}
		}
	}

	void SetFPSCap(double cap)
	{
		if (use_flags_file)
		{
			WriteFlagsFile(cap);
		}
		else
		{
			SetFPSCapInMemory(cap);
		}
	}

	void OnUIClose()
	{
		SetFPSCapInMemory(60.0);
	}

	void OnUnlockMethodUpdate()
	{
		if (Settings::UnlockMethod == Settings::UnlockMethodType::FlagsFile
			|| (Settings::UnlockMethod == Settings::UnlockMethodType::Hybrid && IsLikelyAntiCheatProtected()))
		{
			printf("[%p] Using FlagsFile mode\n", process.handle);
			use_flags_file = true;
			WriteFlagsFile(Settings::FPSCap);
		}
		else
		{
			printf("[%p] Using MemoryWrite mode\n", process.handle);
			if (use_flags_file || IsTargetFpsFlagActive()) WriteFlagsFile(-1);
			use_flags_file = false;
		}
	}
};

std::unordered_map<DWORD, RobloxProcess> AttachedProcesses;

void RFU_SetFPSCap(double value)
{
	for (auto& it : AttachedProcesses)
	{
		it.second.SetFPSCap(value);
	}
}

void RFU_OnUIUnlockMethodChange()
{
	for (auto &it : AttachedProcesses)
	{
		it.second.OnUnlockMethodUpdate();
	}
}

void RFU_OnUIClose()
{
	for (auto &it : AttachedProcesses)
	{
		it.second.OnUIClose();
	}
}

void pause()
{
	printf("Press enter to continue . . .");
	getchar();
}

DWORD WINAPI WatchThread(LPVOID)
{
	printf("Watch thread started\n");

	while (1)
	{
		{
			auto processes = GetRobloxProcesses(false, Settings::UnlockClient, Settings::UnlockStudio);

			for (auto &process : processes)
			{
				auto id = process.id;
				if (AttachedProcesses.find(id) == AttachedProcesses.end())
				{
					assert(!process.IsOpen());
					process.Open();
					printf("Injecting into new process %p (pid %d)\n", process.handle, id);

					RobloxProcess roblox_process;
					roblox_process.Attach(std::move(process), 5);
					AttachedProcesses[id] = std::move(roblox_process);

					printf("New size: %zu\n", AttachedProcesses.size());
				}
			}
		}

		for (auto it = AttachedProcesses.begin(); it != AttachedProcesses.end();)
		{
			auto &process = it->second.GetHandle();

			DWORD code;
			BOOL result = GetExitCodeProcess(process.handle, &code);

			if (code != STILL_ACTIVE)
			{
				printf("Purging dead process %p (pid %d, code %X)\n", process.handle, GetProcessId(process.handle), code);
				it = AttachedProcesses.erase(it);
				printf("New size: %zu\n", AttachedProcesses.size());
			}
			else
			{
				it->second.Tick();
				it++;
			}
		}

		UI::AttachedProcessesCount = AttachedProcesses.size();

		Sleep(2000);
	}

	return 0;
}

bool CheckRunning()
{
	SingletonMutex = CreateMutexA(NULL, FALSE, "RFUMutex");

	if (!SingletonMutex)
	{
		MessageBoxA(NULL, "Unable to create mutex", "Error", MB_OK);
		return false;
	}

	return GetLastError() == ERROR_ALREADY_EXISTS;
}

int CALLBACK WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	if (!Settings::Init())
	{
		char buffer[64];
		sprintf_s(buffer, "Unable to initiate settings\nGetLastError() = %X", GetLastError());
		MessageBoxA(NULL, buffer, "Error", MB_OK);
		return 0;
	}

	UI::IsConsoleOnly = strstr(lpCmdLine, "--console") != nullptr;

	if (UI::IsConsoleOnly)
	{
		UI::ToggleConsole();

		printf("Waiting for Roblox...\n");

		RobloxProcessHandle process;
		RobloxProcess attacher{};

		do
		{
			Sleep(100);
			process = GetRobloxProcess();
		}
		while (!process.IsValid());

		printf("Found Roblox...\n");
		printf("Attaching...\n");

		if (!attacher.Attach(std::move(process), 0))
		{
			printf("\nERROR: unable to attach to process\n");
			pause();
			return 0;
		}

		printf("\nSuccess! The injector will close in 3 seconds...\n");

		Sleep(3000);

		return 0;
	}
	else
	{
		if (CheckRunning())
		{
			MessageBoxA(NULL, "Roblox FPS Unlocker is already running", "Error", MB_OK);
		}
		else
		{
			if (!Settings::QuickStart)
				UI::ToggleConsole();
			else
				UI::CreateHiddenConsole();

			if (Settings::CheckForUpdates)
			{
				printf("Checking for updates...\n");
				if (CheckForUpdates()) return 0;
			}

			if (!Settings::QuickStart)
			{
				printf("Minimizing to system tray in 2 seconds...\n");
				Sleep(2000);
#ifdef NDEBUG
				UI::ToggleConsole();
#endif
			}

			return UI::Start(hInstance, WatchThread);
		}
	}
} 
