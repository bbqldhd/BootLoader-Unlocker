#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <cstdio>   // 用于_popen/_pclose
#include <cstdlib>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <algorithm>
#include <condition_variable>
#include <memory>
#include <stdexcept>
#include <array>
#include <ctime>
#include <cmath>
#include <map>
#include <set>
#include <queue>
#include <deque>
#include <bitset>
#include <numeric>
#include <limits>
#include <cstdint>  // 确保uint64_t兼容
#include <windows.h> // Windows核心API头文件

// 全局变量（与原逻辑一致）
std::atomic<bool> stop_flag(false);
std::mutex console_mutex;
std::mutex task_mutex;
std::condition_variable cv;
std::atomic<uint64_t> current_code(0);
std::atomic<uint64_t> total_attempts(0);
std::atomic<uint64_t> successful_code(0);
std::atomic<bool> found(false);
std::atomic<int> active_threads(0);

// 硬件信息结构体（与原逻辑一致）
struct HardwareInfo {
    int cpu_cores;
    bool gpu_available;
    bool npu_available;
    
    HardwareInfo() : cpu_cores(0), gpu_available(false), npu_available(false) {}
};

// Windows控制台中断处理函数（替换Linux的signal_handler）
// 处理Ctrl+C事件，设置退出标志并通知所有线程
BOOL WINAPI ConsoleCtrlHandler(DWORD dwCtrlType) {
    if (dwCtrlType == CTRL_C_EVENT) { // 捕获Ctrl+C
        std::lock_guard<std::mutex> lock(console_mutex);
        std::cout << "\n接收到中断信号，正在优雅退出...\n";
        stop_flag = true;
        cv.notify_all(); // 唤醒所有等待的线程
        return TRUE; // 标记事件已处理，避免系统默认终止
    }
    return FALSE; // 其他事件（如关闭窗口）交给系统处理
}

// 执行系统命令并获取输出（Windows版：_popen/_pclose替换popen/pclose）
std::string execute_command(const std::string& cmd) {
    std::array<char, 128> buffer;
    std::string result;
    // Windows下用_popen，模式"r"表示读取命令输出
    std::unique_ptr<FILE, decltype(&_pclose)> pipe(_popen(cmd.c_str(), "r"), _pclose);
    
    if (!pipe) {
        throw std::runtime_error("_popen() 执行失败！可能是命令不存在或权限不足");
    }
    
    // 读取命令输出到缓冲区
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    
    return result;
}

// 检测硬件信息（Windows版：替换Linux的lspci/sysconf）
HardwareInfo detect_hardware() {
    HardwareInfo info;
    
    // 1. 检测CPU核心数（Windows API：GetSystemInfo）
    SYSTEM_INFO sys_info;
    GetSystemInfo(&sys_info);
    info.cpu_cores = sys_info.dwNumberOfProcessors; // 逻辑核心数
    
    // 2. 检测GPU（Windows命令：wmic查询显卡信息）
    try {
        // 命令说明：查询所有显卡名称，/format:list让输出更易解析
        std::string gpu_cmd = "wmic path win32_VideoController get Name /format:list";
        std::string gpu_info = execute_command(gpu_cmd);
        // 判定条件：输出包含"Name="且后面有有效内容（排除空结果）
        if (!gpu_info.empty() && gpu_info.find("Name=") != std::string::npos) {
            info.gpu_available = true;
        }
    } catch (...) {
        info.gpu_available = false;
    }
    
    // 3. 检测NPU（Windows命令：wmic查询包含"NPU"的设备）
    try {
        // 命令说明：查询即插即用设备中名称含"NPU"的设备
        // 注意：Windows下双引号需转义（\\"），否则命令解析错误
        std::string npu_cmd = "wmic path win32_PnPEntity where \"Name like '%NPU%'\" get Name /format:list";
        std::string npu_info = execute_command(npu_cmd);
        if (!npu_info.empty() && npu_info.find("Name=") != std::string::npos) {
            info.npu_available = true;
        }
    } catch (...) {
        info.npu_available = false;
    }
    
    return info;
}

// 尝试解锁bootloader（Windows版：修改fastboot路径为Windows格式）
bool try_unlock_bootloader(uint64_t code) {
    if (stop_flag) return false;
    
    std::stringstream ss;
    // Windows下fastboot需用.exe后缀（假设fastboot.exe在当前目录）
    ss << "fastboot.exe flashing oem unlock " << std::setw(16) << std::setfill('0') << code;
    std::string cmd = ss.str();
    
    try {
        std::string result = execute_command(cmd);
        total_attempts++;
        
        // 检查解锁成功的关键词（与原逻辑一致）
        if (result.find("unlock successful") != std::string::npos || 
            result.find("OKAY") != std::string::npos ||
            result.find("unlocked") != std::string::npos) {
            return true;
        }
    } catch (...) {
        // 命令执行失败（如fastboot未找到、设备未连接）
    }
    
    return false;
}

// 尝试直接解锁（无代码，Windows版：修改fastboot路径）
bool try_direct_unlock() {
    if (stop_flag) return false;
    
    try {
        // Windows下使用fastboot.exe
        std::string result = execute_command("fastboot.exe flashing oem unlock");
        total_attempts++;
        
        // 成功条件与原逻辑一致
        if (result.find("unlock successful") != std::string::npos || 
            result.find("OKAY") != std::string::npos ||
            result.find("unlocked") != std::string::npos) {
            return true;
        }
    } catch (...) {
        // 命令执行失败
    }
    
    return false;
}

// 工作线程函数（与原逻辑完全一致）
void worker_thread(uint64_t start, uint64_t end, int thread_id) {
    active_threads++;
    
    for (uint64_t code = start; code <= end && !stop_flag && !found; ++code) {
        {
            std::lock_guard<std::mutex> lock(task_mutex);
            current_code = code;
        }
        
        if (try_unlock_bootloader(code)) {
            found = true;
            successful_code = code;
            stop_flag = true;
            cv.notify_all();
            break;
        }
        
        // 每尝试1000个代码通知进度更新
        if (code % 1000 == 0) {
            cv.notify_all();
        }
    }
    
    active_threads--;
    cv.notify_all();
}

// 显示进度（Windows版：替换ANSI清屏为system("cls")）
void display_progress(uint64_t total_codes) {
    auto start_time = std::chrono::steady_clock::now();
    
    while (!stop_flag && !found) {
        std::unique_lock<std::mutex> lock(console_mutex);
        
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
        
        uint64_t current = current_code.load();
        double progress = (static_cast<double>(current) / total_codes) * 100.0;
        double attempts_per_sec = elapsed > 0 ? static_cast<double>(total_attempts.load()) / elapsed : 0.0;
        
        // Windows控制台清屏（替换Linux的ANSI转义序列）
        system("cls");
        // 进度显示内容（与原逻辑一致）
        std::cout << "========================================\n";
        std::cout << "      Bootloader 解锁工具（Windows版）\n";
        std::cout << "========================================\n\n";
        std::cout << "当前尝试的解锁码: " << std::setw(16) << std::setfill('0') << current << "\n";
        std::cout << "进度: " << std::fixed << std::setprecision(2) << progress << "%\n";
        std::cout << "已尝试次数: " << total_attempts.load() << "\n";
        std::cout << "尝试速度: " << std::fixed << std::setprecision(2) << attempts_per_sec << " 次/秒\n";
        std::cout << "活跃线程数: " << active_threads.load() << "\n";
        std::cout << "\n按 Ctrl+C 退出程序\n";
        
        lock.unlock();
        // 等待500ms或被通知唤醒（与原逻辑一致）
        cv.wait_for(lock, std::chrono::milliseconds(500));
    }
}

// 主函数（Windows版：注册控制台中断处理）
int main() {
    // 注册Windows控制台中断处理（替换Linux的signal(SIGINT)）
    if (!SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE)) {
        std::cerr << "注册控制台中断处理失败！程序可能无法响应Ctrl+C\n";
        return 1;
    }
    
    // 检测硬件（Windows版函数）
    HardwareInfo hw_info = detect_hardware();
    
    // 显示硬件信息（与原逻辑一致）
    {
        std::lock_guard<std::mutex> lock(console_mutex);
        system("cls"); // Windows清屏
        std::cout << "========================================\n";
        std::cout << "      Bootloader 解锁工具（Windows版）\n";
        std::cout << "========================================\n\n";
        std::cout << "检测到的硬件信息:\n";
        std::cout << "CPU逻辑核心数: " << hw_info.cpu_cores << "\n";
        std::cout << "GPU可用性: " << (hw_info.gpu_available ? "是" : "否") << "\n";
        std::cout << "NPU可用性: " << (hw_info.npu_available ? "是" : "否") << "\n\n";
        std::cout << "正在尝试直接解锁...\n";
    }
    
    // 首先尝试直接解锁（无代码，Windows版函数）
    if (try_direct_unlock()) {
        std::lock_guard<std::mutex> lock(console_mutex);
        std::cout << "\n成功解锁Bootloader! (无需解锁码)\n";
        return 0;
    }
    
    // 直接解锁失败，启动暴力破解（与原逻辑一致）
    {
        std::lock_guard<std::mutex> lock(console_mutex);
        std::cout << "直接解锁失败，开始暴力破解16位数字解锁码...\n";
        std::cout << "这将需要很长时间，请耐心等待...\n\n";
    }
    
    // 总解锁码数量（16位数字：0~9999999999999999）
    const uint64_t total_codes = 10000000000000000ULL;
    
    // 根据CPU核心数创建线程（限制最大16线程，与原逻辑一致）
    int num_threads = hw_info.cpu_cores;
    if (num_threads < 1) num_threads = 1;
    if (num_threads > 16) num_threads = 16;
    
    // 计算每个线程的代码范围（与原逻辑一致）
    uint64_t codes_per_thread = total_codes / num_threads;
    std::vector<std::thread> threads;
    
    // 创建工作线程（与原逻辑一致）
    for (int i = 0; i < num_threads; ++i) {
        uint64_t start = i * codes_per_thread;
        // 最后一个线程处理剩余的代码（避免范围遗漏）
        uint64_t end = (i == num_threads - 1) ? total_codes - 1 : (start + codes_per_thread - 1);
        threads.emplace_back(worker_thread, start, end, i);
    }
    
    // 启动进度显示线程（Windows版函数）
    std::thread progress_thread(display_progress, total_codes);
    
    // 等待所有工作线程完成（与原逻辑一致）
    for (auto& thread : threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    
    // 等待进度线程完成（与原逻辑一致）
    if (progress_thread.joinable()) {
        progress_thread.join();
    }
    
    // 显示最终结果（Windows版：清屏优化）
    {
        std::lock_guard<std::mutex> lock(console_mutex);
        system("cls");
        std::cout << "========================================\n";
        std::cout << "      Bootloader 解锁工具（Windows版）\n";
        std::cout << "========================================\n\n";
        
        if (found) {
            std::cout << "成功找到解锁码!\n";
            std::cout << "解锁码: " << std::setw(16) << std::setfill('0') << successful_code.load() << "\n";
        } else if (stop_flag) {
            std::cout << "程序被用户中断。\n";
        } else {
            std::cout << "未能找到有效的解锁码。\n";
        }
        std::cout << "总尝试次数: " << total_attempts.load() << "\n";
        std::cout << "\n程序结束。\n";
    }
    
    return 0;
}
	/*cpoyright CharryTechnology 2024-2025 保留所有权利
		最终解释权归Charry本人所有*/
