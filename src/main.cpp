#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <chrono>
#include <queue>
#include <condition_variable>
#include <atomic>
#include <curl/curl.h>
#include "json.hpp"

using json = nlohmann::json;
using namespace std;

mutex cout_mutex;
atomic<bool> stop_flag{false};

class ThreadPool {
public:
    ThreadPool(size_t threads) : stop(false) {
        for(size_t i = 0; i < threads; ++i)
            workers.emplace_back([this] {
                for(;;) {
                    function<void()> task;
                    {
                        unique_lock<mutex> lock(this->queue_mutex);
                        this->condition.wait(lock,
                            [this]{ return this->stop || !this->tasks.empty(); });
                        if(this->stop && this->tasks.empty())
                            return;
                        task = move(this->tasks.front());
                        this->tasks.pop();
                    }
                    task();
                }
            });
    }

    template<class F>
    void enqueue(F&& f) {
        {
            unique_lock<mutex> lock(queue_mutex);

            tasks.emplace(forward<F>(f));
        }
        condition.notify_one();
    }

    ~ThreadPool() {
        {
            unique_lock<mutex> lock(queue_mutex);
            stop = true;
        }
        condition.notify_all();
        for(thread &worker : workers)
            worker.join();
    }
private:
    vector<thread> workers;
    queue<function<void()>> tasks;
    mutex queue_mutex;
    condition_variable condition;
    bool stop;
};

vector<string> loadLines(const string& filename) {
    ifstream file(filename);
    vector<string> lines;
    string line;

    if (!file.is_open()) {
        lock_guard<mutex> lock(cout_mutex);
        cout << "[!] Файл " << filename << " не найден!\n";
        return lines;
    }

    while (getline(file, line)) {
        if (!line.empty()) lines.push_back(line);
    }

    return lines;
}

size_t WriteCallback(void* contents, size_t size, size_t nmemb, string* output) {
    output->append((char*)contents, size * nmemb);
    return size * nmemb;
}

bool checkTokenValid(const string& token, const string& proxy = "") {
    CURL* curl = curl_easy_init();
    string response_data;
    struct curl_slist* headers = nullptr;

    headers = curl_slist_append(headers, ("Authorization: " + token).c_str());

    curl_easy_setopt(curl, CURLOPT_URL, "https://discord.com/api/v9/users/@me");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);
    if (!proxy.empty())
        curl_easy_setopt(curl, CURLOPT_PROXY, proxy.c_str());

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        lock_guard<mutex> lock(cout_mutex);
        cout << "[-] Невалидный токен: " << token.substr(0, 10) << "... (" << curl_easy_strerror(res) << ")\n";
        return false;
    }

    auto parsed = json::parse(response_data, nullptr, false);
    if (parsed.is_discarded() || !parsed.contains("id")) {
        lock_guard<mutex> lock(cout_mutex);
        cout << "[-] Невалидный токен: " << token.substr(0, 10) << "... (Invalid response)\n";
        return false;
    }

    lock_guard<mutex> lock(cout_mutex);
    cout << "[+] Валидный токен: " << token.substr(0, 10) << "... (User: " << parsed["username"] << "#" << parsed["discriminator"] << ")\n";
    return true;
}

json makeApiRequest(const string& token, const string& url, const string& proxy = "", const string& post_data = "") {
    CURL* curl = curl_easy_init();
    string response_data;
    struct curl_slist* headers = nullptr;
    long http_code = 0;
    json result;

    headers = curl_slist_append(headers, ("Authorization: " + token).c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);
    if (!proxy.empty())
        curl_easy_setopt(curl, CURLOPT_PROXY, proxy.c_str());
    
    if (!post_data.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data.c_str());
    }

    CURLcode res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        lock_guard<mutex> lock(cout_mutex);
        cout << "[!] Ошибка запроса: " << curl_easy_strerror(res) << "\n";
        return {};
    }

    // Обработка rate limits
    if (http_code == 429) {
        lock_guard<mutex> lock(cout_mutex);
        cout << "[!] Rate limit достигнут, ожидание...\n";
        
        // Парсим JSON для получения времени ожидания
        auto parsed = json::parse(response_data, nullptr, false);
        if (!parsed.is_discarded() && parsed.contains("retry_after")) {
            int retry_after = parsed["retry_after"];
            this_thread::sleep_for(chrono::milliseconds(retry_after + 500));
            return makeApiRequest(token, url, proxy, post_data); // Рекурсивный повтор запроса
        } else {
            this_thread::sleep_for(chrono::seconds(5));
            return makeApiRequest(token, url, proxy, post_data);
        }
    }

    if (http_code != 200) {
        lock_guard<mutex> lock(cout_mutex);
        cout << "[!] HTTP ошибка: " << http_code << "\n";
        return {};
    }

    auto parsed = json::parse(response_data, nullptr, false);
    if (parsed.is_discarded()) {
        lock_guard<mutex> lock(cout_mutex);
        cout << "[!] Не удалось распарсить JSON\n";
        return {};
    }

    return parsed;
}

json getGuilds(const string& token, const string& proxy = "") {
    return makeApiRequest(token, "https://discord.com/api/v9/users/@me/guilds", proxy);
}

json getChannels(const string& token, const string& guild_id, const string& proxy = "") {
    string url = "https://discord.com/api/v9/guilds/" + guild_id + "/channels";
    return makeApiRequest(token, url, proxy);
}

bool sendMessageToChannel(const string& token, const string& channel_id, const string& message, const string& proxy = "") {
    string url = "https://discord.com/api/v9/channels/" + channel_id + "/messages";
    json msg_payload = {{"content", message}};
    auto result = makeApiRequest(token, url, proxy, msg_payload.dump());

    if (!result.empty() && result.contains("id")) {
        lock_guard<mutex> lock(cout_mutex);
        cout << "[+] Отправлено в канал " << channel_id << "\n";
        return true;
    }
    return false;
}

void processToken(const string& token, const string& message, int delay, const string& proxy = "") {
    if (stop_flag) return;
    
    if (!checkTokenValid(token, proxy)) {
        return;
    }

    auto guilds = getGuilds(token, proxy);
    if (!guilds.is_array()) {
        lock_guard<mutex> lock(cout_mutex);
        cout << "[-] Ошибка при получении серверов для токена: " << token.substr(0, 10) << "...\n";
        return;
    }

    for (const auto& guild : guilds) {
        if (stop_flag) return;
        
        string guild_id = guild["id"];
        auto channels = getChannels(token, guild_id, proxy);

        for (const auto& channel : channels) {
            if (stop_flag) return;
            
            if (channel["type"] == 0 && channel.contains("id")) { // только текстовые каналы
                string channel_id = channel["id"];
                sendMessageToChannel(token, channel_id, message, proxy);
                this_thread::sleep_for(chrono::seconds(delay));
            }
        }
    }
}

void signalHandler(int signum) {
    cout << "\nПолучен сигнал прерывания, завершение работы...\n";
    stop_flag = true;
}

int main() {
    signal(SIGINT, signalHandler);

    auto tokens = loadLines("token.txt");
    auto proxies = loadLines("proxy.txt");

    if (tokens.empty()) {
        cout << "Файл с токенами пуст или отсутствует!\n";
        return 0;
    }

    string message;
    int delay;
    int thread_count;

    cout << "Введите сообщение: ";
    getline(cin, message);

    cout << "Задержка между сообщениями (5-10 сек): ";
    cin >> delay;

    cout << "Количество потоков (рекомендуется 3-10): ";
    cin >> thread_count;

    if (delay < 5 || delay > 10) {
        cout << "[!] Рекомендуется 5-10 секунд\n";
    }

    ThreadPool pool(thread_count > 0 ? thread_count : 3);

    for (size_t i = 0; i < tokens.size(); ++i) {
        if (stop_flag) break;
        
        string proxy = (i < proxies.size()) ? proxies[i] : "";
        pool.enqueue([token = tokens[i], message, delay, proxy] {
            processToken(token, message, delay, proxy);
        });
        this_thread::sleep_for(chrono::milliseconds(300));
    }

    return 0;
}
