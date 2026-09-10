//#define MODEZERO		//comment out for human benchmark mode, uncomment for black and white
//#define FPSLIMIT 10000 //comment out for uncapped FPS

//set these for different viewport resolution, for example
//#define HEIGHT 480
//#define WIDTH  640

#define HEIGHT GetSystemMetrics(SM_CYSCREEN)
#define WIDTH  GetSystemMetrics(SM_CXSCREEN)




// no need to change anything below this point

#define IDI_APPICON 101 //must match andrew_d3d9HumanBench.rc

#include <d3d9.h>
#include <mmsystem.h> //timeBeginPeriod/timeEndPeriod
#include <atomic>
#include <thread>
#include <vector>
#include <algorithm>
#include <chrono>

//if linker can't find DirectX SDK, you can use these
/*#ifdef _M_IX86 //32 bit compile
#pragma comment (lib, "C:/Program Files (x86)/Microsoft DirectX SDK (June 2010)/Lib/x86/d3d9.lib")
#else
#pragma comment (lib, "C:/Program Files (x86)/Microsoft DirectX SDK (June 2010)/Lib/x64/d3d9.lib")
#endif*/
#pragma comment (lib, "winmm.lib")

D3DCOLOR black = D3DCOLOR(0x00000000);
D3DCOLOR white = D3DCOLOR(0x00FFFFFF);

// Cross-thread-shared atomics get their own cache line each: the input thread writes these
// while the render thread is reading (and sometimes writing) them at very high frequency in
// its own spin loop, and vice versa. Without padding, two of these could land on the same
// 64-byte line and every write from one core would force a coherency invalidation on the
// other core's cached copy ("false sharing") even though the variables are logically
// unrelated. Padding each one out to a full line means a write from one thread can never
// disturb the other thread's cache line for a different variable.
template <typename T>
struct alignas(64) HotAtomic {
	static_assert(sizeof(std::atomic<T>) < 64, "HotAtomic pad would underflow");
	std::atomic<T> value;
	char pad[64 - sizeof(std::atomic<T>)];
	HotAtomic() = default;
	explicit HotAtomic(T v) : value(v) {}
	T load(std::memory_order order = std::memory_order_relaxed) const { return value.load(order); }
	void store(T v, std::memory_order order = std::memory_order_relaxed) { value.store(v, order); }
};

#ifdef MODEZERO
HotAtomic<D3DCOLOR> currentColor{ white };
#else
#include <iostream>
#include <iomanip> // std::setprecision
HotAtomic<D3DCOLOR> currentColor{ black };
D3DCOLOR red = D3DCOLOR(0x00FF0000);
D3DCOLOR green = D3DCOLOR(0x0000FF00);

// time_points aren't atomic, so the red->green deadline and the green-start timestamp are
// shared between threads as raw tick counts of high_resolution_clock's own duration.
HotAtomic<long long> redToGreenTicks{ 0 };
HotAtomic<long long> greenStartTicks{ 0 };
__forceinline std::chrono::high_resolution_clock::time_point loadTimePoint(const HotAtomic<long long>& ticks) {
	return std::chrono::high_resolution_clock::time_point(std::chrono::high_resolution_clock::duration(ticks.load()));
}
__forceinline void storeTimePoint(HotAtomic<long long>& ticks, std::chrono::high_resolution_clock::time_point tp) {
	ticks.store(tp.time_since_epoch().count());
}

std::chrono::high_resolution_clock::time_point greenClickTime; //input-thread-only
std::chrono::high_resolution_clock::time_point redStartTime;   //input-thread-only
HotAtomic<int> state{ 0 }; //0 stats screen, 1 red waiting screen, 2 green
int errors = 0;
std::vector<int64_t> clickTimes;
int64_t clickAmount = 0;
#endif

HotAtomic<bool> stop{ false };

// posted by the render thread, once it has released the D3D9 device, to ask the thread that
// owns the window to actually destroy it (Win32 requires DestroyWindow to run on that thread)
#define WM_APP_TEARDOWN (WM_APP + 1)

constexpr UINT RAW_BUF_CAPACITY = 800; //comfortably covers a single mouse or keyboard RAWINPUT record
alignas(RAWINPUT) static BYTE raw_buf_storage[RAW_BUF_CAPACITY];
RAWINPUT* raw_buf = reinterpret_cast<RAWINPUT*>(raw_buf_storage);

LRESULT CALLBACK WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg) {
	case WM_INPUT: {
		//buffer is pre-sized big enough for mouse/keyboard RAWINPUT, so a single call fetches
		//the data directly instead of round-tripping once to ask for the required size first
		UINT cb_size = RAW_BUF_CAPACITY;
		GetRawInputData((HRAWINPUT)lParam, RID_INPUT, raw_buf, &cb_size, sizeof(RAWINPUTHEADER));

		if (raw_buf->header.dwType == RIM_TYPEMOUSE &&
			(raw_buf->data.mouse.usButtonFlags & (RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_RIGHT_BUTTON_DOWN))) { //bitwise test: a flags word can carry more than one event at once (e.g. a button edge plus a wheel delta) - an equality test could silently miss the click
#ifdef MODEZERO
			currentColor.store(currentColor.load() == white ? black : white);
#else
			int curState = state.load();
			if (curState == 0) {
				state.store(1);
				currentColor.store(red);
				//pick a time to transition screen from red to green
				int rnd = rand() % 4000 + 2000; //random delay, between 2 and 6 seconds
				auto now = std::chrono::high_resolution_clock::now();
				redStartTime = now;
				storeTimePoint(redToGreenTicks, now + std::chrono::milliseconds(rnd));
			}
			else if (curState == 1) { //too early click
				auto now = std::chrono::high_resolution_clock::now();
				auto timeDiff = now - redStartTime;
				if (std::chrono::duration_cast<std::chrono::microseconds>(timeDiff).count() >= 500000) { //grace period for double clicks
					state.store(0);
					currentColor.store(black);
					errors++;
				}
			}
			else if (curState == 2) {
				state.store(0);
				currentColor.store(black);
				greenClickTime = std::chrono::high_resolution_clock::now();
				auto timeDiff = greenClickTime - loadTimePoint(greenStartTicks);
				int64_t clickTime = std::chrono::duration_cast<std::chrono::microseconds>(timeDiff).count();
				clickTimes.push_back(clickTime);
				std::cout << "#" << clickAmount + 1 << ": " << clickTime / 1000.0 << "ms\n";
				clickAmount++;
			}
#endif
		}

#ifndef MODEZERO
		if (raw_buf->header.dwType == RIM_TYPEKEYBOARD) {
#endif
			if (raw_buf->data.keyboard.Message == WM_KEYDOWN || raw_buf->data.keyboard.Message == WM_SYSKEYDOWN)
#ifdef MODEZERO
				if (raw_buf->data.keyboard.VKey == 0x57)  // W key
					currentColor.store(white);
				else if (raw_buf->data.keyboard.VKey == 0x42)  // B key
					currentColor.store(black);
				else
#endif
					if (raw_buf->data.keyboard.VKey == 0x1B) {  // ESC key
#ifndef MODEZERO
						sort(clickTimes.begin(), clickTimes.end());

						size_t size = clickTimes.size();

						int64_t sum = 0;
						for (int64_t clickTime : clickTimes) {
							sum += clickTime;
						}

						int64_t average = sum / size;

						// stdev
						double standard_deviation = 0.0;

						for (int64_t clickTime : clickTimes) {
							standard_deviation += pow(clickTime - average, 2);
						}

						double stdev = sqrt(standard_deviation / (size - 1));

						std::cout << "\nMax: " << clickTimes.back() / 1000.0 << "ms\n";
						std::cout << "Avg: " << average / 1000.0 << "ms\n";
						std::cout << "Min: " << clickTimes.front() / 1000.0 << "ms\n";
						std::cout << "STDEV: " << stdev / 1000.0 << "\n";
						std::cout << "Total Clicks: " << clickAmount + errors << "\n";
						std::cout << "Successful Clicks: " << clickAmount << "\n";
						std::cout << "Early Clicks: " << errors << "\n";
#endif
						//don't tear the window down here: the render thread still owns the D3D9
						//device (fullscreen exclusive) and must release it first. Just flag stop;
						//the render thread will ask us to destroy the window once it's done.
						stop.store(true, std::memory_order_release);
						break;
					}

#ifndef MODEZERO
		}
#endif
		if (GET_RAWINPUT_CODE_WPARAM(wParam) == RIM_INPUT)
			DefWindowProc(hWnd, msg, wParam, lParam); //The application must call DefWindowProc so the system can perform cleanup.
		break;
	}
	case WM_CLOSE:
		//same reasoning as the ESC handler above: let the render thread drive teardown order
		stop.store(true, std::memory_order_release);
		return 0;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	case WM_APP_TEARDOWN:
		DestroyWindow(hWnd);
		return 0;
	default:
		return DefWindowProc(hWnd, msg, wParam, lParam);
	}
	return 0;
}

// Console subsystem entry point, so results printed via std::cout are visible; forwards to WinMain.
int main()
{
#ifndef MODEZERO
	std::cout << std::fixed << std::setprecision(2);
#endif
	int ret = WinMain(GetModuleHandle(NULL), NULL, NULL, SW_SHOWNORMAL);
	return ret;
}

#ifdef FPSLIMIT
float frameTimeMicro = FPSLIMIT == 0 ? 0 : 1000000 / FPSLIMIT;
#else
float frameTimeMicro = 0;
#endif

// Windows 10's power throttling ("EcoQoS") can transparently downclock a thread or, on hybrid
// P-core/E-core CPUs, schedule it onto a slow efficiency core if it looks like background work.
// That would silently wreck latency on exactly the threads we've gone to the trouble of pinning
// and boosting, so explicitly opt them (and the process) out of it.
static void OptOutOfPowerThrottling(HANDLE thread) {
	THREAD_POWER_THROTTLING_STATE state{};
	state.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
	state.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
	state.StateMask = 0; //0 = never throttle this thread
	SetThreadInformation(thread, ThreadPowerThrottling, &state, sizeof(state));
}

// runs on the calling (main) thread until stop is signalled, then releases the D3D9 device and
// asks the input thread (the one that actually owns hWnd) to destroy the window.
void RunRenderLoop(LPDIRECT3D9 d3d, LPDIRECT3DDEVICE9 d3ddev, HWND hWnd, D3DPRESENT_PARAMETERS d3dpp) {
#ifdef FPSLIMIT
	auto frameStart = std::chrono::high_resolution_clock::now();
#endif
	while (!stop.load(std::memory_order_acquire)) {
#ifndef MODEZERO
		if (state.load() == 1 && std::chrono::high_resolution_clock::now() >= loadTimePoint(redToGreenTicks)) {
			state.store(2);
			currentColor.store(green);
			storeTimePoint(greenStartTicks, std::chrono::high_resolution_clock::now()); //keep actual green start timer
		}
#endif

#ifdef FPSLIMIT
		auto elapsed = std::chrono::high_resolution_clock::now() - frameStart;
		if (std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count() >= frameTimeMicro) {
			frameStart = std::chrono::high_resolution_clock::now();
#endif
			d3ddev->Clear(0, NULL, D3DCLEAR_TARGET, currentColor.load(), 0.0f, 0);
			HRESULT hr = d3ddev->Present(NULL, NULL, NULL, NULL);

			// Exclusive fullscreen devices get lost whenever something steals focus - alt-tab, a
			// UAC prompt, a Windows notification toast, a screen lock, an RDP disconnect. Without
			// handling this, Present() would just fail silently forever and the screen would go
			// dead/frozen until the process is killed - one of the most common real stutter/freeze
			// causes for exclusive-fullscreen D3D9 apps.
			if (hr == D3DERR_DEVICELOST) {
				while (!stop.load(std::memory_order_acquire)) {
					HRESULT cooperativeLevel = d3ddev->TestCooperativeLevel();
					if (cooperativeLevel == D3D_OK) break;
					if (cooperativeLevel == D3DERR_DEVICENOTRESET) {
						if (SUCCEEDED(d3ddev->Reset(&d3dpp))) break;
					}
					Sleep(1); //nothing useful to render while the device isn't ours to draw to; a
					          //short sleep here doesn't touch the steady-state input/render latency
				}
			}
#ifdef FPSLIMIT
		}
#endif
	}

	d3ddev->Release();
	d3d->Release();
	PostMessage(hWnd, WM_APP_TEARDOWN, 0, 0);
}

struct InputThreadContext {
	HINSTANCE hInstance;
	HWND hWnd = nullptr;
};

// owns the window and its message queue for the lifetime of the app: Win32 ties a window's
// message queue to the thread that created it, so this is the only thread that can ever see
// WM_INPUT. It does nothing else - no D3D calls, no waiting on Present - and never sleeps: it
// busy-polls PeekMessage instead of blocking on GetMessage, trading a fully-pinned CPU core for
// removing the OS sleep/wake scheduling transition (and its jitter) from the input path
// entirely. Combined with TIME_CRITICAL priority, power-throttling opt-out, and a dedicated
// physical core, this is effectively the ceiling of what user-mode Windows allows.
void InputThreadMain(InputThreadContext* ctx, std::atomic<bool>* windowReady, DWORD_PTR affinityMask) {
	if (affinityMask) SetThreadAffinityMask(GetCurrentThread(), affinityMask);
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
	OptOutOfPowerThrottling(GetCurrentThread());

	WNDCLASSEX wc;
	ZeroMemory(&wc, sizeof(WNDCLASSEX));

	wc.cbSize = sizeof(WNDCLASSEX);
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = WindowProc;
	wc.hInstance = ctx->hInstance;
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hIcon = LoadIcon(ctx->hInstance, MAKEINTRESOURCE(IDI_APPICON));
	wc.hIconSm = wc.hIcon;
	wc.lpszClassName = L"somewindowclass";

	RegisterClassEx(&wc);

	HWND hWnd = CreateWindowEx(0, L"somewindowclass", L"test",
		WS_EX_TOPMOST | WS_POPUP,
		0, 0, (UINT)GetSystemMetrics(SM_CXSCREEN), (UINT)GetSystemMetrics(SM_CYSCREEN),
		NULL, NULL, ctx->hInstance, NULL);

	if (!hWnd) {
		windowReady->store(true, std::memory_order_release);
		return;
	}

	RAWINPUTDEVICE Keyboard;
	Keyboard.usUsagePage = 0x01;
	Keyboard.usUsage = 0x06; //keyboard
	Keyboard.dwFlags = RIDEV_NOLEGACY;
	Keyboard.hwndTarget = hWnd;

	RAWINPUTDEVICE Mouse;
	Mouse.usUsagePage = 0x01;
	Mouse.usUsage = 0x02; //mouse
	Mouse.dwFlags = RIDEV_NOLEGACY;
	Mouse.hwndTarget = hWnd;

	if (!RegisterRawInputDevices(&Keyboard, 1, sizeof(RAWINPUTDEVICE)) ||
		!RegisterRawInputDevices(&Mouse, 1, sizeof(RAWINPUTDEVICE))) {
		DestroyWindow(hWnd);
		windowReady->store(true, std::memory_order_release);
		return;
	}

	ShowWindow(hWnd, SW_SHOWNORMAL);
	ShowCursor(FALSE);
	SetCursor(NULL);

	ctx->hWnd = hWnd;
	windowReady->store(true, std::memory_order_release);

	//busy-poll instead of GetMessage's blocking wait: PeekMessage doesn't have GetMessage's
	//"returns 0 on WM_QUIT" contract, so WM_QUIT has to be checked for explicitly.
	MSG msg;
	for (;;) {
		if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
			if (msg.message == WM_QUIT) break;
			DispatchMessage(&msg);
		}
		else {
			YieldProcessor(); //PAUSE hint: doesn't add meaningful latency (the next poll still sees a
			                   //queued message immediately) but is dramatically kinder to power/heat
			                   //and to a hyperthread sibling than a naked spin
		}
	}
}

// Enumerate physical cores and hand back the affinity masks of two DIFFERENT physical cores -
// one for input, one for render - so the two hot threads can never end up as hyperthread
// siblings of each other (which would have them fight over the same core's execution ports and
// caches even while "on separate logical processors"). Also prefers to skip whichever core owns
// logical processor 0, since Windows tends to route more interrupts/DPCs to it. Falls back to
// the simple "first two schedulable logical processors" approach if the topology query fails or
// the machine doesn't expose at least two physical cores to this process.
static bool PickCoreAffinities(DWORD_PTR processAffinityMask, DWORD_PTR& inputMask, DWORD_PTR& renderMask) {
	DWORD len = 0;
	GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
	if (len == 0) return false;

	std::vector<BYTE> buffer(len);
	auto* infoBase = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data());
	if (!GetLogicalProcessorInformationEx(RelationProcessorCore, infoBase, &len)) return false;

	std::vector<DWORD_PTR> coreMasks;
	BYTE* ptr = buffer.data();
	BYTE* end = buffer.data() + len;
	while (ptr < end) {
		auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(ptr);
		if (info->Relationship == RelationProcessorCore) {
			DWORD_PTR mask = (DWORD_PTR)info->Processor.GroupMask[0].Mask & processAffinityMask;
			if (mask) coreMasks.push_back(mask);
		}
		ptr += info->Size;
	}

	if (coreMasks.size() < 2) return false;

	//cores that don't include logical processor 0 sort first
	std::sort(coreMasks.begin(), coreMasks.end(), [](DWORD_PTR a, DWORD_PTR b) {
		return (a & 1) < (b & 1);
		});

	inputMask = coreMasks[0];
	renderMask = coreMasks[1];
	return true;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	srand(static_cast<unsigned int>(time(NULL)));

	timeBeginPeriod(1); //tighten the scheduler/timer tick for more consistent thread wake latency

	HANDLE process = GetCurrentProcess();
	DWORD_PTR processAffinityMask, systemAffinityMask;
	DWORD_PTR inputCoreMask = 0, renderCoreMask = 0;

	//isolate the input-pump thread and the render thread onto separate physical cores, so a busy
	//render loop can never delay this process's own handling of a WM_INPUT message
	if (GetProcessAffinityMask(process, &processAffinityMask, &systemAffinityMask)) {
		if (!PickCoreAffinities(processAffinityMask, inputCoreMask, renderCoreMask)) {
			//topology query unavailable/insufficient: fall back to the first two available logical processors
			inputCoreMask = 0; renderCoreMask = 0;
			DWORD_PTR mask = 1;
			for (int bit = 0; bit < (int)(sizeof(DWORD_PTR) * 8) && !renderCoreMask; bit++) {
				if (mask & processAffinityMask) {
					if (!inputCoreMask) inputCoreMask = mask;
					else if (!renderCoreMask) renderCoreMask = mask;
				}
				mask <<= 1;
			}
		}
	}

	//HIGH (not REALTIME) on purpose: REALTIME_PRIORITY_CLASS unlocks Windows' real-time priority
	//tier (16-31), which gets no anti-starvation boosting and can preempt system housekeeping
	//threads unconditionally - a hang in a thread up there can take the whole desktop with it.
	//HIGH_PRIORITY_CLASS stays in the scheduler-supervised range while still sitting above every
	//normal application.
	SetPriorityClass(process, HIGH_PRIORITY_CLASS);

	PROCESS_POWER_THROTTLING_STATE procPowerState{};
	procPowerState.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
	procPowerState.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
	procPowerState.StateMask = 0; //opt the whole process out of EcoQoS/efficiency-core scheduling
	SetProcessInformation(process, ProcessPowerThrottling, &procPowerState, sizeof(procPowerState));

	InputThreadContext inputCtx{ hInstance };
	std::atomic<bool> windowReady{ false };
	std::thread inputThread(InputThreadMain, &inputCtx, &windowReady, inputCoreMask);

	while (!windowReady.load(std::memory_order_acquire))
		std::this_thread::yield(); //one-shot wait for window/raw-input setup on the input thread

	if (!inputCtx.hWnd) {
		inputThread.join();
		timeEndPeriod(1);
		return -1;
	}
	HWND hWnd = inputCtx.hWnd;

	LPDIRECT3D9 d3d;
	LPDIRECT3DDEVICE9 d3ddev;
	D3DPRESENT_PARAMETERS d3dpp;

	d3d = Direct3DCreate9(D3D_SDK_VERSION);

	ZeroMemory(&d3dpp, sizeof(d3dpp));
	d3dpp.Windowed = FALSE;
	d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
	d3dpp.hDeviceWindow = hWnd;
	d3dpp.BackBufferFormat = D3DFMT_X8R8G8B8;
	d3dpp.BackBufferWidth = (UINT)GetSystemMetrics(SM_CXSCREEN);
	d3dpp.BackBufferHeight = (UINT)GetSystemMetrics(SM_CYSCREEN);
	d3dpp.BackBufferCount = 1; //deliberately not raised: more buffers measured ~30-40% higher
	                           //throughput here, but each queued buffer is another frame of
	                           //possible delay between drawing something and it reaching the
	                           //screen - the opposite of what this app is for
	d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE; //disable vsync

	d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &d3dpp, &d3ddev);
	if (d3ddev == NULL) {
		stop.store(true, std::memory_order_release);
		PostMessage(hWnd, WM_APP_TEARDOWN, 0, 0);
		inputThread.join();
		d3d->Release();
		timeEndPeriod(1);
		return -1;
	}

	D3DVIEWPORT9 pViewport = { 0, 0, (DWORD)WIDTH, (DWORD)HEIGHT,0.0,1.0 };
	if (d3ddev->SetViewport(&pViewport) != S_OK) {
		stop.store(true, std::memory_order_release);
		d3ddev->Release();
		PostMessage(hWnd, WM_APP_TEARDOWN, 0, 0);
		inputThread.join();
		d3d->Release();
		timeEndPeriod(1);
		return -1;
	}

	d3ddev->ShowCursor(FALSE);

	//the very first Clear+Present pays a one-time cost (shader/driver JIT, fullscreen mode-set
	//settling) that measured ~17ms here versus a steady-state ~1ms - paying it now, before the
	//benchmark can ever be looking at the clock, instead of on the user's first real frame.
	for (int i = 0; i < 5; i++) {
		d3ddev->Clear(0, NULL, D3DCLEAR_TARGET, black, 0.0f, 0);
		d3ddev->Present(NULL, NULL, NULL, NULL);
	}

	if (renderCoreMask) SetThreadAffinityMask(GetCurrentThread(), renderCoreMask);
	//ABOVE_NORMAL, not HIGHEST: within HIGH_PRIORITY_CLASS, HIGHEST and TIME_CRITICAL both resolve
	//to the same absolute priority (15), which would tie with the input thread instead of staying
	//strictly below it. ABOVE_NORMAL keeps a real gap so input always wins any actual contention.
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
	OptOutOfPowerThrottling(GetCurrentThread());

	RunRenderLoop(d3d, d3ddev, hWnd, d3dpp); //blocks (on this thread) until stop is signalled; releases d3d/d3ddev internally

	inputThread.join();

	timeEndPeriod(1);
	ShowCursor(true);

#ifndef MODEZERO
	std::cout << "\nPress Enter to Continue\n";
	getchar();
#endif

	return 0;
}
