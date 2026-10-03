// ============================================================
//  LAN AI Chat Server — Windows 控制台程序
//  功能：
//   1. 局域网 HTTP 服务器（默认 9090，占用则每次 +5 重试）
//   2. 独立 HTML 网页（web/index.html，不嵌入本程序）
//   3. 调用 llama.cpp (llama-server) 运行本地 GGUF 模型
//   4. 同一时间仅一个用户对话，其余排队
//   5. 终端输出日志（访问网址、错误信息）
//  编译：静态链接，无需任何运行库，任意 Windows 电脑可直接运行
// ============================================================
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <deque>
#include <map>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <functional>
#include <memory>
#include <ctime>
#include <random>

#include "json.hpp"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

// ---------------- 配置 ----------------
struct Config {
    int port = 9090;
    int ctxSize = 4096;
    int maxTokens = 2048;
    bool mtpEnabled = false; // MTP（多 token 预测）投机解码，需要模型自带 MTP 头
    int mtpDraftN = 3;       // 每次草稿预测的 token 数
    std::string modelFile = "DeepSeek-R1-Distill-Qwen-1.5B-Q3_K_M.gguf";
};
static Config g_cfg;

static std::string g_exeDir;
static std::string g_webDir;
static std::string g_modelPath;
static std::string g_llamaExe;
static int g_httpPort = 0;
static int g_llamaPort = 0;

static std::atomic<bool> g_shutdown{false};
static std::atomic<bool> g_llamaReady{false};
static std::mutex g_readyMtx;
static std::condition_variable g_readyCv;

static HANDLE g_llamaProc = nullptr;
static HANDLE g_job = nullptr;
static HANDLE g_llamaPipe = nullptr;

static std::chrono::steady_clock::time_point g_startTime = std::chrono::steady_clock::now();

// 最近一次完成的 token 用量（用于 /api/status 的上下文占用估算）
static std::atomic<double> g_lastPromptTokens{0};
static std::atomic<double> g_lastCompletionTokens{0};

// ---------------- 日志 ----------------
static std::mutex g_logMtx;
static std::ofstream g_logFile;

static std::string nowStr() {
    auto t = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(t);
    std::tm tmv;
    localtime_s(&tmv, &tt);
    char buf[32];
    strftime(buf, sizeof buf, "%H:%M:%S", &tmv);
    return buf;
}

static void log(const std::string& msg) {
    std::lock_guard<std::mutex> g(g_logMtx);
    std::string line = "[" + nowStr() + "] " + msg;
    if (g_logFile.is_open()) {
        g_logFile << line << "\n";
        g_logFile.flush();
    }
    // 控制台：优先用 Unicode API 输出，保证中文在任何代码页下都正常显示
    int wlen = MultiByteToWideChar(CP_UTF8, 0, line.c_str(), (int)line.size(), nullptr, 0);
    if (wlen > 0) {
        std::vector<wchar_t> wbuf(wlen + 1);
        MultiByteToWideChar(CP_UTF8, 0, line.c_str(), (int)line.size(), wbuf.data(), wlen);
        wbuf[wlen] = L'\0';
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD written = 0;
        if (hOut && hOut != INVALID_HANDLE_VALUE &&
            WriteConsoleW(hOut, wbuf.data(), (DWORD)wlen, &written, nullptr) &&
            WriteConsoleW(hOut, L"\r\n", 2, &written, nullptr)) {
            return;
        }
    }
    // 输出被重定向（如管道）时退回 UTF-8 文本输出
    printf("%s\n", line.c_str());
    fflush(stdout);
}

// ---------------- 基础工具 ----------------
static std::string exeDir() {
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0) return "";
    std::string p(buf, n);
    size_t pos = p.find_last_of("\\/");
    return pos == std::string::npos ? "" : p.substr(0, pos);
}

static bool fileExists(const std::string& p) {
    DWORD attr = GetFileAttributesA(p.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static std::string toLower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

static std::string urlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int h = hex(s[i + 1]), l = hex(s[i + 2]);
            if (h >= 0 && l >= 0) {
                out += (char)(h * 16 + l);
                i += 2;
                continue;
            }
        }
        if (s[i] == '+') out += ' ';
        else out += s[i];
    }
    return out;
}

static std::string randomHex(size_t len) {
    static std::mt19937_64 rng(std::random_device{}());
    static const char* hex = "0123456789abcdef";
    std::string s;
    s.reserve(len);
    for (size_t i = 0; i < len; i++) s += hex[(rng() >> (i % 8 * 4)) & 0xF];
    return s;
}

static std::string joinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (a.back() == '\\' || a.back() == '/') return a + b;
    return a + "\\" + b;
}

// 粗略 token 估算：中文每字约 1 token，其他每 4 字节约 1 token
static size_t estTokens(const std::string& s) {
    size_t cjk = 0, other = 0;
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            other++;
            i++;
            continue;
        }
        unsigned cp = 0;
        int len = 0;
        if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 3; }
        else { i++; continue; }
        if (i + (size_t)len >= n) break;
        for (int k = 1; k <= len; k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        i += len + 1;
        if (cp >= 0x4E00 && cp <= 0x9FFF) cjk++;
        else other++;
    }
    return cjk + other / 4 + 1;
}

// 读取配置文件 config.ini（键=值，可选）
static void loadConfig() {
    std::string path = joinPath(g_exeDir, "config.ini");
    std::ifstream f(path);
    if (!f.is_open()) return;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t pos = line.find('=');
        if (pos == std::string::npos) continue;
        std::string key = toLower(line.substr(0, pos));
        std::string val = line.substr(pos + 1);
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        val.erase(0, val.find_first_not_of(" \t"));
        val.erase(val.find_last_not_of(" \t") + 1);
        if (key.empty() || val.empty() || val[0] == '#' || val[0] == ';') continue;
        try {
            if (key == "port") g_cfg.port = std::stoi(val);
            else if (key == "ctxsize" || key == "ctx_size") g_cfg.ctxSize = std::stoi(val);
            else if (key == "maxtokens" || key == "max_tokens") g_cfg.maxTokens = std::stoi(val);
            else if (key == "mtp") g_cfg.mtpEnabled = (std::stoi(val) != 0);
            else if (key == "mtpdraft" || key == "mtp_draft" || key == "mtptokens")
                g_cfg.mtpDraftN = std::stoi(val);
            else if (key == "model" || key == "modelfile" || key == "model_file") g_cfg.modelFile = val;
        } catch (...) {
            log("[警告] config.ini 中无效的配置值: " + line);
        }
    }
    if (g_cfg.ctxSize < 512) g_cfg.ctxSize = 512;
    if (g_cfg.maxTokens < 64) g_cfg.maxTokens = 64;
    if (g_cfg.maxTokens > g_cfg.ctxSize - 128) g_cfg.maxTokens = g_cfg.ctxSize - 128;
    if (g_cfg.port < 1 || g_cfg.port > 65535) g_cfg.port = 9090;
    if (g_cfg.mtpDraftN < 1) g_cfg.mtpDraftN = 1;
    if (g_cfg.mtpDraftN > 8) g_cfg.mtpDraftN = 8;
}

// ---------------- Winsock 工具 ----------------
static void setRecvTimeout(SOCKET s, int ms) {
    DWORD t = (DWORD)ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof t);
}

static bool sendAll(SOCKET s, const char* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        int n = send(s, data + off, (int)std::min<size_t>(len - off, 1 << 20), 0);
        if (n <= 0) return false;
        off += (size_t)n;
    }
    return true;
}

static SOCKET tcpConnect(const std::string& ip, int port, int timeoutMs) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
    int rc = connect(s, (sockaddr*)&addr, sizeof addr);
    if (rc == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) { closesocket(s); return INVALID_SOCKET; }
        fd_set w, e;
        FD_ZERO(&w); FD_ZERO(&e);
        FD_SET(s, &w); FD_SET(s, &e);
        timeval tv{ timeoutMs / 1000, (long)(timeoutMs % 1000) * 1000 };
        rc = select(0, nullptr, &w, &e, &tv);
        if (rc <= 0 || FD_ISSET(s, &e)) { closesocket(s); return INVALID_SOCKET; }
    }
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
    return s;
}

// ---------------- 端口探测 ----------------
static bool canBind(int port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((u_short)port);
    bool ok = bind(s, (sockaddr*)&addr, sizeof addr) == 0;
    closesocket(s);
    return ok;
}

// 从 start 开始找空闲端口，每次 +5
static int findFreePort(int start) {
    for (int i = 0; i < 2000; i++) {
        int p = start + i * 5;
        if (p > 65530) break;
        if (canBind(p)) return p;
        log("端口 " + std::to_string(p) + " 已被占用，尝试 " + std::to_string(p + 5) + " ...");
    }
    return -1;
}

// 随机找一个空闲端口（供 llama-server 使用）
static int pickFreePort() {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return 0;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = 0;
    if (bind(s, (sockaddr*)&addr, sizeof addr) != 0) { closesocket(s); return 0; }
    int len = sizeof addr;
    getsockname(s, (sockaddr*)&addr, &len);
    int p = ntohs(addr.sin_port);
    closesocket(s);
    return p;
}

// ---------------- 局域网 IP ----------------
static std::vector<std::string> lanIps() {
    std::vector<std::string> out;
    ULONG size = 16 * 1024;
    std::vector<BYTE> buf(size);
    ULONG rc = GetAdaptersAddresses(AF_INET,
        GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, nullptr,
        (PIP_ADAPTER_ADDRESSES)buf.data(), &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        rc = GetAdaptersAddresses(AF_INET,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, nullptr,
            (PIP_ADAPTER_ADDRESSES)buf.data(), &size);
    }
    if (rc != NO_ERROR) return out;
    PIP_ADAPTER_ADDRESSES a = (PIP_ADAPTER_ADDRESSES)buf.data();
    for (; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        for (auto u = a->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            sockaddr_in* in = (sockaddr_in*)u->Address.lpSockaddr;
            char ip[64];
            inet_ntop(AF_INET, &in->sin_addr, ip, sizeof ip);
            std::string s = ip;
            if (s == "127.0.0.1" || s == "0.0.0.0" || s.rfind("169.254.", 0) == 0) continue;
            if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
        }
    }
    return out;
}

// ---------------- llama-server 进程管理 ----------------
static std::string buildLlamaArgs() {
    std::ostringstream oss;
    oss << "\"" << g_llamaExe << "\"";
    oss << " -m \"" << g_modelPath << "\"";
    oss << " --host 127.0.0.1";
    oss << " --port " << g_llamaPort;
    oss << " --ctx-size " << g_cfg.ctxSize;
    oss << " --parallel 1";
    oss << " --temp 0.6";
    oss << " --top-p 0.95";
    oss << " --reasoning-budget 768";
    if (g_cfg.mtpEnabled) {
        oss << " --spec-type draft-mtp";
        oss << " --spec-draft-n-max " << g_cfg.mtpDraftN;
    }
    oss << " --alias DeepSeek-R1-1.5B";
    return oss.str();
}

static bool spawnLlama() {
    if (g_job == nullptr) {
        g_job = CreateJobObjectA(nullptr, nullptr);
        if (g_job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
            jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &jeli, sizeof jeli);
        }
    }

    SECURITY_ATTRIBUTES sa{ sizeof sa, nullptr, TRUE };
    HANDLE outRead = nullptr, outWrite = nullptr;
    if (!CreatePipe(&outRead, &outWrite, &sa, 0)) {
        log("[错误] 创建管道失败");
        return false;
    }
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);

    std::string cmdline = buildLlamaArgs();
    log("[启动] " + cmdline);

    STARTUPINFOA si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = outWrite;
    si.hStdError = outWrite;
    si.hStdInput = nullptr;

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessA(nullptr, (LPSTR)cmdline.c_str(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, g_exeDir.c_str(), &si, &pi);
    if (!ok) {
        log("[错误] 无法启动 llama-server.exe（错误码 " + std::to_string(GetLastError()) + "）");
        CloseHandle(outRead);
        CloseHandle(outWrite);
        return false;
    }
    if (g_job) AssignProcessToJobObject(g_job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(outWrite);

    if (g_llamaProc) CloseHandle(g_llamaProc);
    g_llamaProc = pi.hProcess;
    if (g_llamaPipe) CloseHandle(g_llamaPipe);
    g_llamaPipe = outRead;

    // 读取 llama 输出并转发到日志
    std::thread([outRead]() {
        std::string pending;
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(outRead, buf, sizeof buf, &n, nullptr) && n > 0) {
            pending.append(buf, n);
            size_t pos;
            while ((pos = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, pos);
                pending.erase(0, pos + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty()) log("[llama] " + line);
            }
        }
        CloseHandle(outRead);
    }).detach();
    return true;
}

// llama-server HTTP 请求（阻塞，短超时）
static json::Value llamaGetJson(const std::string& path, int timeoutMs = 3000) {
    SOCKET s = tcpConnect("127.0.0.1", g_llamaPort, timeoutMs);
    if (s == INVALID_SOCKET) return json::Value();
    setRecvTimeout(s, timeoutMs);
    std::string req = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" +
        std::to_string(g_llamaPort) + "\r\nConnection: close\r\n\r\n";
    if (!sendAll(s, req.data(), req.size())) { closesocket(s); return json::Value(); }

    std::string resp;
    char buf[8192];
    int n;
    while ((n = recv(s, buf, sizeof buf, 0)) > 0) resp.append(buf, n);
    closesocket(s);
    if (resp.empty()) return json::Value();

    size_t hdrEnd = resp.find("\r\n\r\n");
    if (hdrEnd == std::string::npos) return json::Value();
    std::string headers = resp.substr(0, hdrEnd);
    if (headers.find("200") == std::string::npos) return json::Value();

    std::string body = resp.substr(hdrEnd + 4);
    // 处理 chunked
    if (toLower(headers).find("transfer-encoding: chunked") != std::string::npos) {
        std::string decoded;
        size_t i = 0;
        while (i < body.size()) {
            size_t le = body.find("\r\n", i);
            if (le == std::string::npos) break;
            std::string sz = body.substr(i, le - i);
            size_t semi = sz.find(';');
            if (semi != std::string::npos) sz = sz.substr(0, semi);
            long chunkLen = strtol(sz.c_str(), nullptr, 16);
            if (chunkLen <= 0) break;
            i = le + 2;
            if (i + chunkLen > body.size()) break;
            decoded.append(body, i, chunkLen);
            i += chunkLen + 2;
        }
        body = decoded;
    }
    try {
        return json::parse(body);
    } catch (...) {
        return json::Value();
    }
}

// 流式 POST（SSE），onData 逐块回调；返回是否完整成功
static bool llamaStreamPost(const std::string& path, const std::string& body,
                            const std::function<void(const std::string&)>& onData,
                            const std::atomic<bool>* cancel) {
    SOCKET s = tcpConnect("127.0.0.1", g_llamaPort, 3000);
    if (s == INVALID_SOCKET) return false;
    setRecvTimeout(s, 1000);

    std::string req = "POST " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" +
        std::to_string(g_llamaPort) + "\r\nContent-Type: application/json\r\nContent-Length: " +
        std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    if (!sendAll(s, req.data(), req.size())) { closesocket(s); return false; }

    // 读响应头
    std::string hdr;
    char buf[8192];
    int idleMs = 0;
    while (hdr.find("\r\n\r\n") == std::string::npos) {
        if (cancel && cancel->load()) { closesocket(s); return false; }
        int n = recv(s, buf, sizeof buf, 0);
        if (n > 0) {
            hdr.append(buf, n);
            idleMs = 0;
            if (hdr.size() > 65536) { closesocket(s); return false; }
        } else if (n == 0) {
            closesocket(s);
            return false;
        } else {
            idleMs += 1000;
            if (idleMs > 60000) { closesocket(s); return false; }
        }
    }
    size_t hdrEnd = hdr.find("\r\n\r\n");
    std::string headers = hdr.substr(0, hdrEnd);
    bool okStatus = headers.find("200") != std::string::npos;
    if (!okStatus) {
        // 非 200：把错误体读出来记日志
        std::string errBody;
        int n;
        while ((n = recv(s, buf, sizeof buf, 0)) > 0) errBody.append(buf, n);
        closesocket(s);
        log("[错误] AI 引擎返回错误: " + headers.substr(0, 200) + " | " + errBody.substr(0, 300));
        return false;
    }
    std::string rest = hdr.substr(hdrEnd + 4);
    bool chunked = toLower(headers).find("transfer-encoding: chunked") != std::string::npos;

    // 读 body（支持 chunked）
    auto feedDecoded = [&](const std::string& data) {
        if (!data.empty()) onData(data);
    };

    std::string chunkBuf = rest;
    idleMs = 0;
    if (chunked) {
        size_t expect = 0;
        bool inData = false;
        bool ended = false;
        auto process = [&]() {
            while (!ended) {
                if (!inData) {
                    size_t le = chunkBuf.find("\r\n");
                    if (le == std::string::npos) return;
                    std::string sz = chunkBuf.substr(0, le);
                    size_t semi = sz.find(';');
                    if (semi != std::string::npos) sz = sz.substr(0, semi);
                    expect = strtol(sz.c_str(), nullptr, 16);
                    chunkBuf.erase(0, le + 2);
                    if (expect == 0) { ended = true; return; }
                    inData = true;
                }
                if (chunkBuf.size() >= expect + 2) {
                    feedDecoded(chunkBuf.substr(0, expect));
                    chunkBuf.erase(0, expect + 2);
                    inData = false;
                } else {
                    return;
                }
            }
        };
        while (!ended) {
            process();
            if (ended) break;
            if (cancel && cancel->load()) { closesocket(s); return false; }
            int n = recv(s, buf, sizeof buf, 0);
            if (n > 0) { chunkBuf.append(buf, n); idleMs = 0; }
            else if (n == 0) break;
            else {
                idleMs += 1000;
                if (idleMs > 300000) { closesocket(s); return false; }
            }
        }
    } else {
        int n;
        while ((n = recv(s, buf, sizeof buf, 0)) != 0) {
            if (n > 0) {
                feedDecoded(std::string(buf, n));
                idleMs = 0;
            } else {
                if (cancel && cancel->load()) { closesocket(s); return false; }
                idleMs += 1000;
                if (idleMs > 300000) { closesocket(s); return false; }
            }
        }
    }
    closesocket(s);
    return !(cancel && cancel->load());
}

// 等待 llama-server 就绪
static bool waitLlamaReady(int timeoutSec) {
    auto t0 = std::chrono::steady_clock::now();
    while (!g_shutdown.load()) {
        if (WaitForSingleObject(g_llamaProc, 0) == WAIT_OBJECT_0) return false;
        json::Value h = llamaGetJson("/health", 1500);
        if (h.type == json::Type::Object && h["status"].getStr() == "ok") return true;
        auto el = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count();
        if (el > timeoutSec) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    return false;
}

// 监视 llama-server：退出后自动重启
static void llamaMonitor() {
    int restarts = 0;
    for (;;) {
        if (WaitForSingleObject(g_llamaProc, 0) == WAIT_OBJECT_0) {
            if (g_shutdown.load()) return;
            DWORD code = 0;
            GetExitCodeProcess(g_llamaProc, &code);
            log("[警告] AI 引擎 (llama-server) 已退出，退出码 " + std::to_string(code));
            g_llamaReady.store(false);
            if (++restarts > 20) {
                log("[错误] AI 引擎重启次数过多，已停止重试。请检查模型文件与内存。");
                while (!g_shutdown.load()) std::this_thread::sleep_for(std::chrono::milliseconds(500));
                return;
            }
            log("[提示] 3 秒后自动重启 AI 引擎 ...");
            std::this_thread::sleep_for(std::chrono::seconds(3));
            if (g_shutdown.load()) return;
            if (!spawnLlama()) {
                log("[错误] 重启 AI 引擎失败");
                while (!g_shutdown.load()) std::this_thread::sleep_for(std::chrono::milliseconds(500));
                return;
            }
            if (waitLlamaReady(240)) {
                restarts = 0;
                g_llamaReady.store(true);
                g_readyCv.notify_all();
                log("[状态] AI 引擎已就绪");
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
    }
}

// ---------------- 任务队列 ----------------
struct ChatMsg {
    std::string role;
    std::string content;
};

struct Job {
    std::string ticket;
    std::string client;
    std::vector<ChatMsg> messages;
    int maxTokens = 2048;
    std::atomic<int> state{0}; // 0 排队中 1 生成中 2 完成 3 错误 4 取消
    std::string result;
    std::string error;
    double promptTokens = 0;
    double completionTokens = 0;
    bool trimmed = false;
    int removed = 0;
    bool truncated = false; // 达到最大长度被截断
    std::atomic<bool> cancel{false};
    std::chrono::steady_clock::time_point created = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point started;
    std::chrono::steady_clock::time_point doneAt;
    std::mutex mtx;
};

static std::mutex g_queueMtx;
static std::condition_variable g_queueCv;
static std::deque<std::shared_ptr<Job>> g_queue; // 队首 = 当前处理中

static std::mutex g_resultsMtx;
static std::map<std::string, std::shared_ptr<Job>> g_results; // 已完成任务（按 ticket）

// llama 状态缓存（供 /api/status，1 秒刷新一次）
static std::mutex g_slotsMtx;
static json::Value g_slotsCache;
static json::Value g_propsCache;
static std::chrono::steady_clock::time_point g_slotsTime;

static void refreshLlamaStatus() {
    std::lock_guard<std::mutex> g(g_slotsMtx);
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - g_slotsTime).count() < 1000) return;
    g_slotsTime = now;
    if (!g_llamaReady.load()) {
        g_slotsCache = json::Value();
        g_propsCache = json::Value();
        return;
    }
    g_slotsCache = llamaGetJson("/slots", 1500);
    g_propsCache = llamaGetJson("/props", 1500);
}

// 执行一个任务
static void runJob(std::shared_ptr<Job> job) {
    log("[任务] 开始生成 client=" + job->client.substr(0, 8) +
        " 消息数=" + std::to_string(job->messages.size()));

    // 1) 超出上下文时裁剪最早的对话（保留 system）
    std::vector<ChatMsg> msgs = job->messages;
    size_t limit = (size_t)g_cfg.ctxSize > (size_t)job->maxTokens + 512
        ? (size_t)g_cfg.ctxSize - (size_t)job->maxTokens - 512
        : (size_t)g_cfg.ctxSize / 2;
    size_t total = 0;
    for (auto& m : msgs) total += estTokens(m.content);
    while (msgs.size() > 1 && total > limit) {
        int idx = -1;
        for (size_t i = 0; i < msgs.size(); i++) {
            if (msgs[i].role != "system") { idx = (int)i; break; }
        }
        if (idx < 0) break;
        total -= estTokens(msgs[idx].content);
        msgs.erase(msgs.begin() + idx);
        job->removed++;
    }
    if (job->removed > 0) {
        job->trimmed = true;
        log("[任务] 对话超出上下文，自动省略了最早的 " + std::to_string(job->removed) + " 条消息");
    }

    // 2) 构造请求
    json::Value req = json::Value::mkObject();
    json::Value arr = json::Value::mkArray();
    for (auto& m : msgs) {
        json::Value o = json::Value::mkObject();
        (*o.obj)["role"] = m.role;
        (*o.obj)["content"] = m.content;
        arr.arr->push_back(o);
    }
    (*req.obj)["messages"] = arr;
    (*req.obj)["stream"] = true;
    (*req.obj)["max_tokens"] = (double)job->maxTokens;
    (*req.obj)["temperature"] = 0.6;
    (*req.obj)["top_p"] = 0.95;
    json::Value so = json::Value::mkObject();
    (*so.obj)["include_usage"] = true;
    (*req.obj)["stream_options"] = so;
    std::string body = json::dump(req);

    // 3) 流式请求
    std::string sseBuf;
    bool inThink = false;
    bool ok = llamaStreamPost("/v1/chat/completions", body, [&](const std::string& data) {
        sseBuf += data;
        size_t pos;
        while ((pos = sseBuf.find('\n')) != std::string::npos) {
            std::string line = sseBuf.substr(0, pos);
            sseBuf.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("data: ", 0) != 0) continue;
            std::string payload = line.substr(6);
            if (payload == "[DONE]") continue;
            try {
                json::Value v = json::parse(payload);
                const json::Value& choices = v["choices"];
                if (choices.type == json::Type::Array && choices.size() > 0) {
                    const json::Value& delta = choices.at(0)["delta"];
                    std::string piece;
                    bool addThinkOpen = false, addThinkClose = false;
                    const json::Value& rc = delta["reasoning_content"];
                    if (rc.type == json::Type::String && !rc.str.empty()) {
                        if (!inThink) { addThinkOpen = true; inThink = true; }
                        piece = rc.str;
                    }
                    const json::Value& ct = delta["content"];
                    if (ct.type == json::Type::String && !ct.str.empty()) {
                        if (inThink) { addThinkClose = true; inThink = false; }
                        if (!addThinkOpen) piece = ct.str; else piece += ct.str;
                    }
                    if (!piece.empty() || addThinkOpen || addThinkClose) {
                        std::lock_guard<std::mutex> g(job->mtx);
                        if (addThinkOpen) job->result += "<thinking>";
                        if (!piece.empty()) job->result += piece;
                        if (addThinkClose) job->result += "</thinking>";
                    }
                    const json::Value& fr = choices.at(0)["finish_reason"];
                    if (fr.type == json::Type::String && fr.str == "length") {
                        job->truncated = true;
                    }
                }
                if (v.has("usage")) {
                    std::lock_guard<std::mutex> g(job->mtx);
                    job->promptTokens = v["usage"]["prompt_tokens"].getNum(0);
                    job->completionTokens = v["usage"]["completion_tokens"].getNum(0);
                }
            } catch (...) {
                // 忽略无法解析的行
            }
        }
    }, &job->cancel);

    {
        std::lock_guard<std::mutex> g(job->mtx);
        if (inThink) job->result += "</thinking>";
    }

    job->doneAt = std::chrono::steady_clock::now();
    if (!ok) {
        if (job->cancel.load()) {
            job->state.store(4);
            log("[任务] 已取消 ticket=" + job->ticket);
        } else if (!g_shutdown.load()) {
            job->state.store(3);
            job->error = "与 AI 引擎通信中断，请稍后重试";
            log("[错误] 任务失败 ticket=" + job->ticket + "：与 AI 引擎通信中断");
        } else {
            job->state.store(3);
        }
        return;
    }
    if (job->cancel.load()) {
        job->state.store(4);
        log("[任务] 已取消 ticket=" + job->ticket);
        return;
    }
    job->state.store(2);
    g_lastPromptTokens.store(job->promptTokens);
    g_lastCompletionTokens.store(job->completionTokens);
    double secs = std::chrono::duration_cast<std::chrono::milliseconds>(job->doneAt - job->started).count() / 1000.0;
    log("[任务] 完成 ticket=" + job->ticket +
        " 用时=" + std::to_string((int)secs) + "s" +
        " 输出=" + std::to_string((int)job->completionTokens) + " tokens" +
        (job->trimmed ? " (已裁剪历史)" : ""));
}

// 工作线程：串行处理队列
static void workerMain() {
    for (;;) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock<std::mutex> lk(g_queueMtx);
            g_queueCv.wait(lk, [] { return !g_queue.empty() || g_shutdown.load(); });
            if (g_shutdown.load()) return;
            job = g_queue.front();
            job->state.store(1);
            job->started = std::chrono::steady_clock::now();
        }
        {
            std::unique_lock<std::mutex> lk(g_readyMtx);
            g_readyCv.wait(lk, [] { return g_llamaReady.load() || g_shutdown.load(); });
            if (g_shutdown.load()) return;
        }
        runJob(job);
        {
            std::lock_guard<std::mutex> g1(g_queueMtx);
            if (!g_queue.empty() && g_queue.front() == job) g_queue.pop_front();
            std::lock_guard<std::mutex> g2(g_resultsMtx);
            g_results[job->ticket] = job;
        }
    }
}

// 通过 ticket 或 client 查找任务（先队列后结果）
static std::shared_ptr<Job> findJob(const std::string& ticket, const std::string& client) {
    {
        std::lock_guard<std::mutex> g(g_queueMtx);
        for (auto& j : g_queue) {
            if ((!ticket.empty() && j->ticket == ticket) ||
                (!client.empty() && j->client == client)) return j;
        }
    }
    {
        std::lock_guard<std::mutex> g(g_resultsMtx);
        if (!ticket.empty()) {
            auto it = g_results.find(ticket);
            if (it != g_results.end()) return it->second;
        }
        if (!client.empty()) {
            for (auto& kv : g_results) {
                if (kv.second->client == client) return kv.second;
            }
        }
    }
    return nullptr;
}

// ---------------- HTTP 服务器 ----------------
static std::string statusText(int code) {
    switch (code) {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 500: return "Internal Server Error";
        default: return "Unknown";
    }
}

static void sendResponse(SOCKET s, int code, const std::string& ctype,
                         const std::string& body, const std::string& extra = "") {
    std::ostringstream h;
    h << "HTTP/1.1 " << code << " " << statusText(code) << "\r\n";
    h << "Content-Type: " << ctype << "\r\n";
    h << "Content-Length: " << body.size() << "\r\n";
    h << "Connection: close\r\n";
    if (!extra.empty()) h << extra << "\r\n";
    h << "\r\n";
    std::string head = h.str();
    sendAll(s, head.data(), head.size());
    if (!body.empty()) sendAll(s, body.data(), body.size());
}

static void sendJson(SOCKET s, int code, const json::Value& v) {
    sendResponse(s, code, "application/json; charset=utf-8", json::dump(v), "Cache-Control: no-cache");
}

static const char* contentTypeFor(const std::string& path) {
    std::string ext;
    size_t dot = path.find_last_of('.');
    if (dot != std::string::npos) ext = toLower(path.substr(dot));
    if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".js" || ext == ".mjs") return "application/javascript; charset=utf-8";
    if (ext == ".json") return "application/json; charset=utf-8";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif") return "image/gif";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".ico") return "image/x-icon";
    if (ext == ".woff2") return "font/woff2";
    if (ext == ".woff") return "font/woff";
    if (ext == ".txt" || ext == ".md") return "text/plain; charset=utf-8";
    if (ext == ".map") return "application/json";
    return "application/octet-stream";
}

static bool readFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// 安全拼接静态文件路径（防目录穿越）
static bool safeWebPath(const std::string& reqPath, std::string& out) {
    std::string p = urlDecode(reqPath);
    if (p.empty() || p[0] != '/') return false;
    std::vector<std::string> parts;
    std::string cur;
    for (char c : p) {
        if (c == '/' || c == '\\') {
            if (!cur.empty()) { parts.push_back(cur); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    out.clear();
    for (auto& part : parts) {
        if (part == "." || part.empty()) continue;
        if (part == "..") return false;
        if (!out.empty()) out += "\\";
        out += part;
    }
    return true;
}

static void handleStatic(SOCKET s, const std::string& reqPath) {
    std::string rel;
    if (!safeWebPath(reqPath, rel)) {
        sendResponse(s, 404, "text/plain; charset=utf-8", "404 Not Found");
        return;
    }
    if (rel.empty() || rel == "index.html") rel = "index.html";
    std::string path = joinPath(g_webDir, rel);
    std::string content;
    if (!fileExists(path) || !readFile(path, content)) {
        sendResponse(s, 404, "text/plain; charset=utf-8", "404 Not Found");
        return;
    }
    std::string cc = rel == "index.html" ? "Cache-Control: no-cache" : "Cache-Control: max-age=3600";
    sendResponse(s, 200, contentTypeFor(path), content, cc);
}

// GET /api/status?client=xxx
static void handleStatus(SOCKET s, const std::string& query) {
    std::string client;
    size_t pos = query.find("client=");
    if (pos != std::string::npos) {
        client = query.substr(pos + 7);
        size_t amp = client.find('&');
        if (amp != std::string::npos) client = client.substr(0, amp);
    }

    refreshLlamaStatus();

    int nCtx = g_cfg.ctxSize;
    int nPast = 0;
    {
        std::lock_guard<std::mutex> g(g_slotsMtx);
        if (g_slotsCache.type == json::Type::Array && g_slotsCache.size() > 0) {
            nCtx = (int)g_slotsCache.at(0)["n_ctx"].getInt(g_cfg.ctxSize);
        }
    }
    // 上下文占用估算：空闲时用上次完成的精确值；生成中动态估算
    {
        std::lock_guard<std::mutex> g(g_queueMtx);
        if (!g_queue.empty()) {
            auto front = g_queue.front();
            if (front->state.load() == 1) {
                size_t pt = 0;
                for (auto& m : front->messages) pt += estTokens(m.content);
                std::lock_guard<std::mutex> g2(front->mtx);
                nPast = (int)(pt + estTokens(front->result));
            }
        }
    }
    if (nPast == 0) {
        nPast = (int)(g_lastPromptTokens.load() + g_lastCompletionTokens.load());
    }

    json::Value r = json::Value::mkObject();
    (*r.obj)["model"] = g_cfg.modelFile;
    (*r.obj)["llama_ok"] = g_llamaReady.load();
    (*r.obj)["ctx_size"] = (double)nCtx;
    (*r.obj)["n_past"] = (double)nPast;
    (*r.obj)["uptime_s"] = (double)std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - g_startTime).count();
    (*r.obj)["port"] = (double)g_httpPort;
    json::Value ips = json::Value::mkArray();
    for (auto& ip : lanIps()) ips.arr->push_back(ip);
    (*r.obj)["ips"] = ips;

    int queueLen = 0;
    int yourPos = -1;
    json::Value active = json::Value();
    {
        std::lock_guard<std::mutex> g(g_queueMtx);
        if (!g_queue.empty()) {
            auto front = g_queue.front();
            queueLen = (int)g_queue.size() - 1;
            active = json::Value::mkObject();
            (*active.obj)["client"] = front->client.substr(0, 8);
            (*active.obj)["elapsed_s"] = (double)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - front->started).count() / 1000.0;
            {
                std::lock_guard<std::mutex> g2(front->mtx);
                (*active.obj)["tokens_so_far"] = (double)estTokens(front->result);
            }
        }
        int idx = 0;
        for (auto& j : g_queue) {
            if (!client.empty() && j->client == client) {
                yourPos = (idx == 0) ? 0 : idx;
                break;
            }
            idx++;
        }
    }
    (*r.obj)["queue_len"] = (double)queueLen;
    (*r.obj)["active"] = active;
    (*r.obj)["your_position"] = (double)yourPos;

    sendJson(s, 200, r);
}

// GET /api/result?ticket=xxx
static void handleResult(SOCKET s, const std::string& query) {
    std::string ticket;
    size_t pos = query.find("ticket=");
    if (pos != std::string::npos) {
        ticket = query.substr(pos + 7);
        size_t amp = ticket.find('&');
        if (amp != std::string::npos) ticket = ticket.substr(0, amp);
    }
    // 清理过期结果
    {
        std::lock_guard<std::mutex> g(g_resultsMtx);
        if (g_results.size() > 32) {
            auto now = std::chrono::steady_clock::now();
            for (auto it = g_results.begin(); it != g_results.end();) {
                if (std::chrono::duration_cast<std::chrono::seconds>(now - it->second->doneAt).count() > 600)
                    it = g_results.erase(it);
                else
                    ++it;
            }
        }
    }
    std::shared_ptr<Job> job = findJob(ticket, "");
    if (!job) {
        json::Value e = json::Value::mkObject();
        (*e.obj)["state"] = "error";
        (*e.obj)["error"] = "任务不存在或已过期";
        sendJson(s, 200, e);
        return;
    }

    json::Value r = json::Value::mkObject();
    int st = job->state.load();
    if (st == 0) {
        int position = 0;
        {
            std::lock_guard<std::mutex> g(g_queueMtx);
            int idx = 0;
            for (auto& j : g_queue) {
                if (j == job) { position = idx; break; }
                idx++;
            }
        }
        (*r.obj)["state"] = "queued";
        (*r.obj)["position"] = (double)position;
    } else {
        std::string text;
        double pt, ct;
        {
            std::lock_guard<std::mutex> g(job->mtx);
            text = job->result;
            pt = job->promptTokens;
            ct = job->completionTokens;
        }
        if (st == 1) {
            (*r.obj)["state"] = "generating";
            (*r.obj)["text"] = text;
        } else if (st == 2) {
            (*r.obj)["state"] = "done";
            (*r.obj)["text"] = text;
            (*r.obj)["prompt_tokens"] = pt;
            (*r.obj)["completion_tokens"] = ct;
            (*r.obj)["truncated"] = job->truncated;
        } else if (st == 3) {
            (*r.obj)["state"] = "error";
            (*r.obj)["text"] = text;
            (*r.obj)["error"] = job->error.empty() ? "生成失败" : job->error;
        } else {
            (*r.obj)["state"] = "cancelled";
            (*r.obj)["text"] = text;
        }
        (*r.obj)["trimmed"] = job->trimmed;
        (*r.obj)["removed"] = (double)job->removed;
        (*r.obj)["elapsed_s"] = (double)std::chrono::duration_cast<std::chrono::milliseconds>(
            job->doneAt - job->started).count() / 1000.0;
    }
    sendJson(s, 200, r);
}

// POST /api/chat
static void handleChat(SOCKET s, const std::string& body) {
    json::Value req;
    try {
        req = json::parse(body);
    } catch (const std::exception& e) {
        json::Value err = json::Value::mkObject();
        (*err.obj)["error"] = std::string("请求格式错误: ") + e.what();
        sendJson(s, 400, err);
        return;
    }
    std::string client = req["client"].getStr();
    if (client.empty() || client.size() > 64) {
        json::Value err = json::Value::mkObject();
        (*err.obj)["error"] = "缺少 client 参数";
        sendJson(s, 400, err);
        return;
    }
    const json::Value& messages = req["messages"];
    if (messages.type != json::Type::Array || messages.size() == 0) {
        json::Value err = json::Value::mkObject();
        (*err.obj)["error"] = "messages 不能为空";
        sendJson(s, 400, err);
        return;
    }
    std::vector<ChatMsg> msgs;
    for (size_t i = 0; i < messages.size(); i++) {
        const json::Value& m = messages.at(i);
        std::string role = m["role"].getStr();
        std::string content = m["content"].getStr();
        if (role.empty() || content.empty()) continue;
        if (role != "system" && role != "user" && role != "assistant") continue;
        msgs.push_back({ role, content });
    }
    if (msgs.empty()) {
        json::Value err = json::Value::mkObject();
        (*err.obj)["error"] = "messages 格式错误";
        sendJson(s, 400, err);
        return;
    }
    int maxTokens = (int)req["max_tokens"].getInt(g_cfg.maxTokens);
    if (maxTokens < 64) maxTokens = 64;
    if (maxTokens > 8192) maxTokens = 8192;
    if (maxTokens > g_cfg.ctxSize - 128) maxTokens = g_cfg.ctxSize - 128;

    // 该用户是否已有排队/生成中的请求（只查队列，不查已完成结果）
    {
        std::lock_guard<std::mutex> g(g_queueMtx);
        for (auto& j : g_queue) {
            if (j->client == client) {
                json::Value err = json::Value::mkObject();
                (*err.obj)["error"] = "您已有一条消息在排队或生成中，请等待完成";
                sendJson(s, 409, err);
                return;
            }
        }
    }

    auto job = std::make_shared<Job>();
    job->ticket = randomHex(16);
    job->client = client;
    job->messages = msgs;
    job->maxTokens = maxTokens;

    int position;
    {
        std::lock_guard<std::mutex> g(g_queueMtx);
        position = (int)g_queue.size();
        g_queue.push_back(job);
    }
    g_queueCv.notify_one();
    log("[请求] 收到消息 client=" + client.substr(0, 8) +
        " 消息数=" + std::to_string(msgs.size()) +
        " 排队位置=" + std::to_string(position));

    json::Value r = json::Value::mkObject();
    (*r.obj)["ticket"] = job->ticket;
    (*r.obj)["position"] = (double)position;
    sendJson(s, 200, r);
}

// POST /api/cancel  {"ticket": "..."} 或 {"client": "..."}
static void handleCancel(SOCKET s, const std::string& body) {
    json::Value req;
    try {
        req = json::parse(body);
    } catch (...) {
        json::Value err = json::Value::mkObject();
        (*err.obj)["error"] = "请求格式错误";
        sendJson(s, 400, err);
        return;
    }
    std::string ticket = req["ticket"].getStr();
    std::string client = req["client"].getStr();

    // 从队列移除（仅未开始的任务）
    std::shared_ptr<Job> found;
    {
        std::lock_guard<std::mutex> g(g_queueMtx);
        for (auto it = g_queue.begin(); it != g_queue.end(); ++it) {
            auto& j = *it;
            bool match = (!ticket.empty() && j->ticket == ticket) ||
                         (!client.empty() && j->client == client);
            if (!match) continue;
            if (j->state.load() == 0 && it != g_queue.begin()) {
                found = j;
                g_queue.erase(it);
                break;
            }
            if (j->state.load() == 0 && it == g_queue.begin()) {
                // 已出队但还没开始，直接取消
                found = j;
                g_queue.erase(it);
                break;
            }
            // 生成中 → 请求取消
            if (j->state.load() == 1) {
                j->cancel.store(true);
                found = j;
                break;
            }
        }
    }
    if (found) {
        if (found->state.load() == 0) {
            found->state.store(4);
            found->doneAt = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> g(g_resultsMtx);
            g_results[found->ticket] = found;
            log("[任务] 排队中的请求已取消 ticket=" + found->ticket);
        } else {
            log("[任务] 正在请求取消生成 ticket=" + found->ticket);
        }
    }

    json::Value r = json::Value::mkObject();
    (*r.obj)["ok"] = found != nullptr;
    if (!found) (*r.obj)["error"] = "未找到可取消的请求";
    sendJson(s, 200, r);
}

// 处理单个客户端连接
static void handleConn(SOCKET s) {
    setRecvTimeout(s, 10000);
    std::string req;
    char buf[16384];
    int n;
    while (req.find("\r\n\r\n") == std::string::npos && req.size() < 131072) {
        n = recv(s, buf, sizeof buf, 0);
        if (n <= 0) {
            closesocket(s);
            return;
        }
        req.append(buf, n);
    }
    if (req.find("\r\n\r\n") == std::string::npos) {
        closesocket(s);
        return;
    }
    size_t hdrEnd = req.find("\r\n\r\n");
    std::string header = req.substr(0, hdrEnd);
    std::string rest = req.substr(hdrEnd + 4);

    // 解析请求行
    size_t lineEnd = header.find("\r\n");
    std::string reqLine = header.substr(0, lineEnd);
    std::istringstream iss(reqLine);
    std::string method, target, version;
    iss >> method >> target >> version;

    // 解析 Content-Length
    size_t contentLen = 0;
    {
        std::string lower = toLower(header);
        size_t pos = lower.find("content-length:");
        if (pos != std::string::npos) {
            contentLen = (size_t)atoll(header.c_str() + pos + 15);
        }
    }
    if (contentLen > 8 * 1024 * 1024) {
        sendResponse(s, 400, "text/plain; charset=utf-8", "body too large");
        closesocket(s);
        return;
    }
    std::string body = rest;
    while (body.size() < contentLen) {
        n = recv(s, buf, (int)std::min<size_t>(sizeof buf, contentLen - body.size()), 0);
        if (n <= 0) {
            closesocket(s);
            return;
        }
        body.append(buf, n);
    }

    // 路径与查询
    std::string path = target;
    std::string query;
    size_t qpos = target.find('?');
    if (qpos != std::string::npos) {
        path = target.substr(0, qpos);
        query = target.substr(qpos + 1);
    }

    if (method == "POST" && path == "/api/chat") {
        handleChat(s, body);
    } else if (method == "POST" && path == "/api/cancel") {
        handleCancel(s, body);
    } else if (method == "GET" && path == "/api/status") {
        handleStatus(s, query);
    } else if (method == "GET" && path == "/api/result") {
        handleResult(s, query);
    } else if (method == "GET" && path == "/api/ping") {
        json::Value r = json::Value::mkObject();
        (*r.obj)["ok"] = true;
        (*r.obj)["llama_ok"] = g_llamaReady.load();
        sendJson(s, 200, r);
    } else if (method == "GET") {
        handleStatic(s, path);
    } else if (method == "POST" && path == "/favicon.ico") {
        sendResponse(s, 404, "text/plain", "404");
    } else {
        sendResponse(s, 405, "text/plain; charset=utf-8", "405 Method Not Allowed");
    }
    closesocket(s);
}

// HTTP 服务器主循环
static void serverMain(int port) {
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) {
        log("[错误] 创建监听 socket 失败");
        return;
    }
    int opt = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof opt);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((u_short)port);
    if (bind(ls, (sockaddr*)&addr, sizeof addr) != 0) {
        log("[错误] 绑定端口 " + std::to_string(port) + " 失败（错误码 " + std::to_string(WSAGetLastError()) + "）");
        closesocket(ls);
        return;
    }
    if (listen(ls, SOMAXCONN) != 0) {
        log("[错误] listen 失败");
        closesocket(ls);
        return;
    }
    log("[状态] HTTP 服务已启动，监听端口 " + std::to_string(port));
    for (;;) {
        sockaddr_in ca{};
        int cl = sizeof ca;
        SOCKET cs = accept(ls, (sockaddr*)&ca, &cl);
        if (cs == INVALID_SOCKET) {
            if (g_shutdown.load()) break;
            log("[错误] accept 失败（错误码 " + std::to_string(WSAGetLastError()) + "）");
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        std::thread(handleConn, cs).detach();
    }
    closesocket(ls);
}

// ---------------- 主函数 ----------------
int main() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    g_startTime = std::chrono::steady_clock::now();
    g_exeDir = exeDir();
    g_webDir = joinPath(g_exeDir, "web");
    g_llamaExe = joinPath(g_exeDir, "llama\\llama-server.exe");
    g_modelPath = joinPath(g_exeDir, "models\\DeepSeek-R1-Distill-Qwen-1.5B-Q3_K_M.gguf");

    loadConfig();
    if (!g_cfg.modelFile.empty()) {
        // 允许 config.ini 里直接写文件名
        if (g_cfg.modelFile.find('\\') == std::string::npos && g_cfg.modelFile.find('/') == std::string::npos)
            g_modelPath = joinPath(g_exeDir, "models\\" + g_cfg.modelFile);
        else
            g_modelPath = g_cfg.modelFile;
    }

    // 打开日志文件；若文件为空则写入 UTF-8 BOM，方便记事本识别中文
    {
        std::string logPath = joinPath(g_exeDir, "server.log");
        std::ifstream chk(logPath, std::ios::binary | std::ios::ate);
        bool empty = true;
        if (chk.is_open()) { empty = (chk.tellg() == 0); chk.close(); }
        g_logFile.open(logPath, std::ios::app | std::ios::binary);
        if (g_logFile.is_open() && empty) g_logFile.write("\xEF\xBB\xBF", 3);
    }

    log("==================================================");
    log("  局域网 AI 聊天服务器  v1.0");
    log("  模型: " + g_cfg.modelFile);
    log("==================================================");

    if (!fileExists(g_llamaExe)) {
        log("[错误] 找不到 llama-server.exe: " + g_llamaExe);
        log("[错误] 请确认程序目录下的 llama 文件夹完整（需包含 llama-server.exe 及全部 DLL）");
        printf("\n按回车键退出...\n");
        fflush(stdout);
        getchar();
        return 1;
    }
    if (!fileExists(g_modelPath)) {
        log("[错误] 找不到模型文件: " + g_modelPath);
        log("[错误] 请将 GGUF 模型文件放入 models 文件夹");
        printf("\n按回车键退出...\n");
        fflush(stdout);
        getchar();
        return 1;
    }
    if (!fileExists(joinPath(g_webDir, "index.html"))) {
        log("[错误] 找不到网页文件: " + joinPath(g_webDir, "index.html"));
        log("[错误] 请确认 web 文件夹包含 index.html");
        printf("\n按回车键退出...\n");
        fflush(stdout);
        getchar();
        return 1;
    }

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        log("[错误] Winsock 初始化失败");
        printf("\n按回车键退出...\n");
        fflush(stdout);
        getchar();
        return 1;
    }

    // 找 HTTP 端口
    g_httpPort = findFreePort(g_cfg.port);
    if (g_httpPort < 0) {
        log("[错误] 找不到可用端口");
        return 1;
    }
    log("HTTP 端口: " + std::to_string(g_httpPort) + (g_httpPort != g_cfg.port ? " (默认端口被占用，已自动调整)" : ""));

    // 找 llama 内部端口
    g_llamaPort = pickFreePort();
    if (g_llamaPort <= 0) {
        log("[错误] 无法为 AI 引擎分配端口");
        return 1;
    }

    // 启动 llama-server
    log("正在启动 AI 引擎 (llama-server) ...");
    if (!spawnLlama()) {
        log("[错误] AI 引擎启动失败");
        printf("\n按回车键退出...\n");
        fflush(stdout);
        getchar();
        return 1;
    }

    // 等待引擎就绪（模型加载约 2~10 秒）
    log("正在加载模型，请稍候 ...");
    if (!waitLlamaReady(240)) {
        log("[错误] AI 引擎启动超时或失败，请检查:");
        log("       1. llama 文件夹是否完整（llama-server.exe 及所有 DLL）");
        log("       2. 模型文件是否完整（" + g_modelPath + "）");
        log("       3. 内存是否充足（至少 2GB 空闲）");
        log("       详细原因见上方 [llama] 输出");
        printf("\n按回车键退出...\n");
        fflush(stdout);
        getchar();
        g_shutdown.store(true);
        return 1;
    }
    g_llamaReady.store(true);
    g_readyCv.notify_all();
    log("[状态] AI 引擎已就绪，模型加载完成");

    // 监视线程（引擎崩溃自动重启）
    std::thread monitorThr(llamaMonitor);

    // 打印访问地址
    log("------------------------------------------");
    log("  服务器已启动，局域网内其他电脑可用浏览器访问：");
    log("    本机访问:  http://127.0.0.1:" + std::to_string(g_httpPort));
    for (auto& ip : lanIps()) {
        log("    局域网访问: http://" + ip + ":" + std::to_string(g_httpPort));
    }
    log("  （若其他电脑无法访问，请检查 Windows 防火墙是否放行）");
    log("------------------------------------------");
    log("提示: 输入 quit 或 q 可退出；输入 status 可查看队列状态");

    // 启动工作线程与 HTTP 服务器
    std::thread workerThr(workerMain);
    std::thread serverThr(serverMain, g_httpPort);

    // 控制台命令线程（stdin 关闭不影响服务器运行）
    std::thread inputThr([]() {
        std::string cmd;
        while (!g_shutdown.load() && std::getline(std::cin, cmd)) {
            std::string c = toLower(cmd);
            if (c == "quit" || c == "q" || c == "exit") {
                g_shutdown.store(true);
                break;
            } else if (c == "status" || c == "s") {
                std::lock_guard<std::mutex> g(g_queueMtx);
                log("[状态] 队列长度=" + std::to_string(g_queue.size()) +
                    " 引擎=" + std::string(g_llamaReady.load() ? "就绪" : "离线"));
                if (!g_queue.empty()) {
                    auto f = g_queue.front();
                    log("[状态]   当前: client=" + f->client.substr(0, 8) +
                        " 状态=" + std::to_string(f->state.load()));
                }
            } else if (c == "help" || c == "h") {
                log("命令: quit/q 退出 | status/s 查看状态 | help 帮助");
            } else if (!c.empty()) {
                log("[提示] 未知命令: " + cmd + "（输入 help 查看帮助）");
            }
        }
    });
    inputThr.detach();

    // 主线程等待退出
    while (!g_shutdown.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    log("正在退出 ...");
    g_queueCv.notify_all();
    g_readyCv.notify_all();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    return 0;
}
