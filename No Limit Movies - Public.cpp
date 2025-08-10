#include <iostream>
#include <string>
#include <map>
#include <sstream>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <thread>
#include <string.h>
#include <iomanip>
#include "NoLimitMovies.h"
#include <fstream>
#include <unordered_set>
#include <Windows.h>
#include <filesystem>
#include <fcntl.h>
#include <io.h>
#include <ctime>
#include <atomic>
#include <mutex> 
#include <tlhelp32.h>

using namespace httplib;

namespace fs = std::filesystem;

using namespace std;

// Remote Varibles
atomic<bool> skipRequested(false);
atomic<bool> volumeUpRequested(false);
atomic<bool> volumeDownRequested(false);

using std::wstring;

using json = nlohmann::json;

const char API_KEY[] = "YOUR_API_KEY"; // TMDB API Key

const vector<string> movieExtensions = { ".mp4", ".mkv", ".avi", ".mov", ".wmv", ".flv", ".mpeg" };

bool sendPauseCommand() {
    wstring pipeName = L"\\\\.\\pipe\\mpv-pipe";

    HANDLE hPipe = CreateFileW(
        pipeName.c_str(),
        GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL
    );

    if (hPipe == INVALID_HANDLE_VALUE) {
        wcerr << L"[ERROR] Could not open MPV pipe. Error: " << GetLastError() << L"\n";
        return false;
    }

    string json = R"({"command": ["cycle", "pause"]})" "\n";

    DWORD bytesWritten;
    bool success = WriteFile(hPipe, json.c_str(), (DWORD)json.length(), &bytesWritten, NULL)
        && bytesWritten == json.length();

    if (!success) {
        wcerr << L"[ERROR] Failed to write pause command. Error: " << GetLastError() << L"\n";
    }

    CloseHandle(hPipe);
    return success;
}

std::string wstringToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int sizeNeeded = WideCharToMultiByte(
        CP_UTF8, 0,
        wstr.data(), (int)wstr.size(),
        nullptr, 0,
        nullptr, nullptr);
    std::string result(sizeNeeded, 0);
    WideCharToMultiByte(
        CP_UTF8, 0,
        wstr.data(), (int)wstr.size(),
        result.data(), sizeNeeded,
        nullptr, nullptr);
    return result;
}

string escapeJson(const string& str) {
    string escaped;
    for (size_t i = 0; i < str.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(str[i]);

        switch (c) {
        case '\"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (c < 0x20) {
                char buffer[7];
                sprintf_s(buffer, "\\u%04x", c);
                escaped += buffer;
            }
            else {
                escaped += c; // do not split UTF-8 chars!
            }
        }
    }
    return escaped;
}

bool sendShowTextCommand(const wstring& wmessage) {

    const wchar_t* pipeName = L"\\\\.\\pipe\\mpv-pipe";
    HANDLE hPipe = CreateFileW(pipeName, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);

    if (hPipe == INVALID_HANDLE_VALUE) {
        wcerr << L"[ERROR] Could not open MPV pipe. Error: " << GetLastError() << L"\n";
        return false;
    }

    string utf8Message = wstringToUtf8(wmessage);     // Convert from wstring to UTF-8
    string safeMessage = escapeJson(utf8Message);     // Escape for JSON

    //string json = "{\"command\": [\"show-text\", \"" + safeMessage + "\"]}\n";
    int durationMs = 5000; // Show OSD for 10 seconds
    string json = "{\"command\": [\"show-text\", \"" + safeMessage + "\", " + to_string(durationMs) + "]}\n";


    DWORD bytesWritten;
    bool success = WriteFile(hPipe, json.c_str(), (DWORD)json.size(), &bytesWritten, NULL)
        && bytesWritten == json.size();

    if (!success) {
        wcerr << L"[ERROR] Failed to write show-text to MPV. Error: " << GetLastError() << L"\n";
    }

    CloseHandle(hPipe);
    return success;
}

void sendKeyToMPV(WORD vk) {
    HWND hwnd = FindWindowW(NULL, L"mpv");
    if (hwnd) {
        SetForegroundWindow(hwnd);
        PostMessage(hwnd, WM_KEYDOWN, vk, 0);
        PostMessage(hwnd, WM_KEYUP, vk, 0);
    }
}

void handleSleepRoute() {
    sendKeyToMPV(VK_ESCAPE);
    wcout << L"[DEBUG] Sleep mode triggered\n";
    fs::path audioFolder = L"YOUR_AUDIO_FOLDER_PATH"; // use double \\ for seperators

    if (!fs::exists(audioFolder)) {
        wcout << L"[ERROR] Audio folder missing: " << audioFolder << L"\n";
        return;
    }

    vector<fs::path> opusFiles;
    for (const auto& entry : fs::directory_iterator(audioFolder)) {
        if (!entry.is_regular_file()) continue;
        fs::path path = entry.path();
        wstring filename = path.filename().wstring();
        if (filename.find(L"yt-dlp") != wstring::npos) continue;
        if (path.extension() == L".opus") opusFiles.push_back(path);
    }
    if (opusFiles.empty()) {
        wcout << L"[WARN] No valid .opus files found\n";
        return;
    }

    // Pick random file
    random_device rd;
    mt19937 gen(rd());
    uniform_int_distribution<> dist(0, opusFiles.size() - 1);
    fs::path selectedAudio = opusFiles[dist(gen)];
    wcout << L"[INFO] Selected file: " << selectedAudio.wstring() << L"\n";
    currentMoviePlaying = selectedAudio.wstring();

    bool playbackStarted = playAndMonitorMovie(selectedAudio, false);

    if (playbackStarted) {
        wcout << L"[OK] Sleeping. Playing: " << selectedAudio.filename().wstring() << L"\n";
    }
    else {
        wcout << L"[FAIL] Failed to sleep mpv.\n";
    }
}

void startHttpListener() {

    static Server svr;

    svr.Post("/download", [](const Request& req, Response& res) {
        if (!req.has_param("title")) {
            res.status = 400;
            res.set_content("Missing 'title' query parameter\n", "text/plain");
            return;
        }

        std::string movieTitle = req.get_param_value("title");
        std::cerr << "Received download request for: " << movieTitle << "\n";

        try {
            bool ok = downloadByTitle(movieTitle);

            if (ok) {
                res.status = 200;
                res.set_content(movieTitle + " was found. Downloading has started.\n", "text/plain");
            }
            else {
                res.status = 404;
                res.set_content("No matching torrent found for " + movieTitle + "\n", "text/plain");
            }
        }
        catch (const std::exception& e) {
            res.status = 500;
            res.set_content(std::string("Error: ") + e.what(), "text/plain");
        }
        });


    // Handle /skip
    svr.Get("/skip", [](const Request& req, Response& res) {
        wcout << L"Received HTTP request: /skip\n";
        skipRequested = true;
        res.set_content("Skip requested", "text/plain");
        });

    // Handle /volume/up
    svr.Get("/volume/up", [](const Request& req, Response& res) {
        wcout << L"Received HTTP request: /volume/up\n";
        volumeUpRequested = true;
        res.set_content("Volume up requested", "text/plain");
        });

    // Handle /volume/down
    svr.Get("/volume/down", [](const Request& req, Response& res) {
        wcout << L"Received HTTP request: /volume/down\n";
        volumeDownRequested = true;
        res.set_content("Volume down requested", "text/plain");
        });

    // Handle /pause
    svr.Get("/pause", [](const Request& req, Response& res) {
        wcout << L"Received HTTP request: /pause\n";
        sendPauseCommand(); // directly call it
        res.set_content("Pause toggled", "text/plain");
        });

    svr.Get("/info", [](const Request& req, Response& res) {
        wcout << L"[HTTP] GET /info called\n";

        if (currentMoviePlaying.empty()) {
            wcout << L"[INFO] No movie currently playing\n";
            res.status = 404;
            res.set_content("No movie playing", "text/plain");
        }
        else {

            // Extract just filename from full path
            fs::path fullPath(currentMoviePlaying);
            wstring fileName = fullPath.filename().wstring();

            wcout << L"[INFO] Current movie: " << fileName << L"\n";

            bool ok = sendShowTextCommand(fileName);

            if (!ok) {
                wcerr << L"[ERROR] Failed to send show-text to MPV\n";
            }
            else {
                wcout << L"[INFO] Successfully sent OSD\n";
            }

            std::string response = wstringToUtf8(fileName);
            res.set_content(response, "text/plain");
        }
        });

    // Route: /sleep
    svr.Get("/sleep", [](const Request& req, Response& res) {

        wcout << L"[INFO] /sleep HTTP request received\n";

        if (!sleepModeActive) {
            sleepModeActive = true;
            wcout << L"[INFO] /sleep mode ON\n";
            res.set_content("Sleep Mode [ON]", "text/plain");
        }
        else {
            sleepModeActive = false;
            wcout << L"[INFO] /sleep mode OFF\n";
            res.set_content("Sleep Mode [OFF]", "text/plain");
        }

        });


    // Start server thread
    thread([] {
        wcout << L"Starting HTTP server on port 8090...\n";
        if (!svr.listen("0.0.0.0", 8090)) {
            wcerr << L"Failed to start HTTP server! Port may already be in use.\n";
        }
        }).detach();
}

bool startsWithDigits(const string& text, size_t count) {
    if (text.size() < count) {
        // String too short to have 'count' digits
        return false;
    }

    for (size_t i = 0; i < count; ++i) {
        if (!isdigit(text[i])) {
            return false; // Found a non-digit character
        }
    }

    return true; // All first 'count' characters are digits
}

string extractYear(const string& input) {
    if (input.size() < 4) return "";
    return input.substr(0, 4); // First 4 characters
}

string extractTitle(const string& input) {
    if (input.size() <= 5) return "";
    return input.substr(5); // From position 5 onwards (skip "YYYY ")
}

string toLowerCase(const string& input) {
    string result = input;
    transform(result.begin(), result.end(), result.begin(),
        [](unsigned char c) { return tolower(c); });
    return result;
}

double bytesToGB(uint64_t bytes) {
    const double BYTES_IN_GB = 1073741824.0; // 1024^3 bytes
    return static_cast<double>(bytes) / BYTES_IN_GB;
}

struct TorrentResult {
    string title;
    string magnetLink;
    int seeders = 0;
    int leechers = 0;
    uint64_t sizeBytes;
};

size_t WriteCallback(void* contents, size_t size, size_t nmemb, string* output) {
    output->append(static_cast<char*>(contents), size * nmemb);
    return size * nmemb;
}

string url_encode(CURL* curl, const string& str) {
    char* output = curl_easy_escape(curl, str.c_str(), (int)str.length());
    if (output) {
        string encoded(output);
        curl_free(output);
        return encoded;
    }
    return "";
}

string build_query_string(CURL* curl, const map<string, string>& params) {
    ostringstream oss;
    bool first = true;
    for (map<string, string>::const_iterator it = params.begin(); it != params.end(); ++it) {
        const string& key = it->first;
        const string& value = it->second;
        if (!value.empty()) {
            if (!first) oss << "&";
            oss << url_encode(curl, key) << "=" << url_encode(curl, value);
            first = false;
        }
    }
    return oss.str();
}

vector<json> fetch_custom_movies(const map<string, string>& params) {
    const int MAX_PAGES_LIMIT = 1000;
    const int REQUEST_DELAY_MS = 250;

    vector<json> all_movies;
    CURL* curl = curl_easy_init();
    if (!curl) {
        cerr << "Failed to initialize curl." << endl;
        return all_movies;
    }

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    int page = 1;
    int total_pages = 1;

    while (page <= total_pages && page <= MAX_PAGES_LIMIT) {
        map<string, string> query_params = params;
        query_params["api_key"] = API_KEY;
        query_params["page"] = to_string(page);

        string query_string = build_query_string(curl, query_params);
        string endpoint = (params.find("query") != params.end() && !params.at("query").empty())
            ? "https://api.themoviedb.org/3/search/movie?"
            : "https://api.themoviedb.org/3/discover/movie?";
        string url = endpoint + query_string;

        string response;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            cerr << "curl_easy_perform() failed: " << curl_easy_strerror(res) << endl;
            break;
        }

        if (response.empty()) {
            cerr << "Empty response from server.\n";
            break;
        }

        try {
            json parsed = json::parse(response);
            total_pages = parsed.value("total_pages", 1);

            if (!parsed.contains("results") || !parsed["results"].is_array()) {
                cerr << "Invalid results data.\n";
                break;
            }

            for (const auto& movie : parsed["results"]) {
                string title = movie.value("title", "");
                if (title.empty()) {
                    // Skip movies without title
                    continue;
                }
                all_movies.push_back(movie);
            }
        }
        catch (const json::parse_error& e) {
            cerr << "JSON parsing error: " << e.what() << endl;
            break;
        }

        ++page;
        this_thread::sleep_for(chrono::milliseconds(REQUEST_DELAY_MS));
    }

    curl_easy_cleanup(curl);
    return all_movies;
}

vector<TorrentResult> searchJackett(const string& jackettApiKey, const string& query) {

    vector<TorrentResult> results;

    CURL* curl = curl_easy_init();

    if (!curl) {
        cerr << "Failed to initialize cURL.\n";
        return results;
    }

    // Escape query string
    char* encodedQuery = curl_easy_escape(curl, query.c_str(), query.length());
    if (!encodedQuery) {
        cerr << "Failed to encode query\n";
        curl_easy_cleanup(curl);
        return results;
    }

    string url = "http://localhost:9117/api/v2.0/indexers/all/results?apikey=" + jackettApiKey + "&Query=" + encodedQuery;

    curl_free(encodedQuery);  // free immediately after use

    string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    //curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);  // 10 seconds timeout

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        cerr << "Curl error: " << curl_easy_strerror(res) << endl;
        return results;
    }

    try {
        json data = json::parse(response);

        if (!data.contains("Results") || !data["Results"].is_array()) {
            cerr << "Warning: No 'Results' field in Jackett response.\n";
            return results;
        }

        cerr << "Jackett results downloaded successfully, found " << data["Results"].size() << " entries." << endl;

        for (const auto& item : data["Results"]) {
            TorrentResult tr;

            tr.title = (item.contains("Title") && !item["Title"].is_null()) ? item["Title"].get<string>() : "Unknown Title";
            tr.magnetLink = (item.contains("MagnetUri") && !item["MagnetUri"].is_null()) ? item["MagnetUri"].get<string>() : "";
            tr.seeders = (item.contains("Seeders") && !item["Seeders"].is_null()) ? item["Seeders"].get<int>() : 0;
            tr.leechers = (item.contains("Peers") && !item["Peers"].is_null()) ? item["Peers"].get<int>() : 0;
            tr.sizeBytes = (item.contains("Size") && !item["Size"].is_null()) ? item["Size"].get<uint64_t>() : 0ULL;


            if (!tr.magnetLink.empty()) {
                results.push_back(tr);
            }
        }
    }
    catch (const std::exception& e) {
        cerr << "JSON parse error: " << e.what() << endl;
    }

    return results;
}

bool contains_case(const string& str, const string& keyword) {
    auto it = search(str.begin(), str.end(),
        keyword.begin(), keyword.end(),
        [](char ch1, char ch2) { return tolower(ch1) == tolower(ch2); });
    return it != str.end();
}

string remove_non_letters(const string& input) {
    string result;
    for (char c : input) {
        if (isalpha(static_cast<unsigned char>(c))) {
            result.push_back(c);
        }
    }
    return result;
}

string remove_spaces_if_present(const string& input) {
    if (input.find(' ') == string::npos) {
        return input; // No spaces found
    }

    string result;
    for (char c : input) {
        if (c != ' ') {
            result.push_back(c);
        }
    }
    return result;
}

bool findWordInString(const string& title, const string& magnetTitle) {

    string eMagnet = magnetTitle;
    string eTitle = title;

    eMagnet = remove_non_letters(eMagnet);
    eMagnet = remove_spaces_if_present(eMagnet);
    eMagnet = toLowerCase(eMagnet);

    eTitle = remove_non_letters(eTitle);
    eTitle = remove_spaces_if_present(eTitle);
    eTitle = toLowerCase(eTitle);

    size_t ti = 0, pi = 0;

    while (ti < eMagnet.size() && pi < eTitle.size()) {
        char tc = eMagnet[ti];
        char pc = eTitle[pi];

        if (isalpha(static_cast<unsigned char>(tc))) {
            if (tc == pc) {
                pi++; // Found matching letter, advance pattern
            }
        }

        ti++;
    }

    bool matched = (pi == eTitle.size());

    return matched; // True if all pattern letters matched
}

int score_torrent(const TorrentResult& torrent, const string& movieTitle) {
    int score = 0;
    const string& title = torrent.title;

    // === Reject Bad Torrents ===
    if (contains_case(title, "CAM") || contains_case(title, "TS") ||
        contains_case(title, "TELESYNC") || contains_case(title, "HDTS")) {
        return -10000;
    }

    // === Exclude torrents smaller than 5GB ===
    if (torrent.sizeBytes < (5ULL * 1024 * 1024 * 1024)) {
        return -10000;  // Strong penalty to filter out torrents < 5GB
    }

    // === Penalize torrents over 100GB ===
    if (torrent.sizeBytes > (100ULL * 1024 * 1024 * 1024)) {
        return -10000;  // Strong penalty if torrent is over 100GB
    }

    if (torrent.seeders == 0) {
        return -10000; // Hard reject — no point in scoring a dead torrent
    }

    // === Check if torrent title matches movie title ===
    if (findWordInString(movieTitle, title)) {
        score += 100;   // Bonus for good match
    }
    else {
        score -= 5000;   // Optional penalty for poor match
    }

    // === Quality Keywords ===
    if (contains_case(title, "REMUX")) score += 40;
    else if (contains_case(title, "BluRay")) score += 30;
    else if (contains_case(title, "WEB-DL")) score += 20;
    else if (contains_case(title, "WEBRip")) score += 10;

    if (contains_case(title, "2160p")) score += 50;
    else if (contains_case(title, "1080p")) score += 40;
    else if (contains_case(title, "720p")) score += 30;

    if (contains_case(title, "HEVC") || contains_case(title, "H.265")) score += 30;
    else if (contains_case(title, "x264")) score += 10;

    if (contains_case(title, "HDR") || contains_case(title, "Dolby Vision")) score += 20;

    if (contains_case(title, "MKV")) score += 40; // Increased from 10 to 40
    if (contains_case(title, "TrueHD") || contains_case(title, "DTS-HD")) score += 30; // Increased from 15 to 30


    // === Seeder Score ===
    score += torrent.seeders;

    if (torrent.seeders > torrent.leechers && torrent.leechers > 0) {
        int diff = torrent.seeders - torrent.leechers;
        int bonus = diff / 10;   // 1 point per 10 more seeders
        score += bonus;
    }
    else if (torrent.leechers > torrent.seeders) {
        int diff = torrent.leechers - torrent.seeders;
        int penalty = diff / 1 * 10; // Penalize 1 point per 1 more leechers * multiplier
        score -= penalty;
    }

    // === Size Bonus: 1 point per 1 GB over 5GB, no cap
    int sizeMB = torrent.sizeBytes / (1024 * 1024);
    int sizeOver5GB = max(0, sizeMB - 5120);

    int sizeBonus = sizeOver5GB / 1024;
    score += sizeBonus * 3;  // 3 points per GB over 5GB

    return score;
}

bool sendToQbittorrent(const string& qbHost, const string& username, const string& password, const string& magnetLink) {

    CURL* curl = curl_easy_init();
    if (!curl) {
        cerr << "Failed to initialize cURL.\n";
        return false;
    }

    CURLcode res;
    string cookieFile = "cookies.txt";

    // 1. LOGIN
    string loginUrl = qbHost + "/api/v2/auth/login";
    string loginData = "username=" + username + "&password=" + password;

    curl_easy_setopt(curl, CURLOPT_URL, loginUrl.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, loginData.c_str());
    curl_easy_setopt(curl, CURLOPT_COOKIEJAR, cookieFile.c_str()); // Save cookies

    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        cerr << "Login request failed: " << curl_easy_strerror(res) << "\n";
        curl_easy_cleanup(curl);
        return false;
    }

    // 2. ADD MAGNET LINK
    string addUrl = qbHost + "/api/v2/torrents/add";

    char* escapedMagnet = curl_easy_escape(curl, magnetLink.c_str(), static_cast<int>(magnetLink.length()));
    if (!escapedMagnet) {
        cerr << "Failed to escape magnet link.\n";
        curl_easy_cleanup(curl);
        return false;
    }

    string postData = "urls=" + string(escapedMagnet);
    curl_free(escapedMagnet); // Free after use

    curl_easy_setopt(curl, CURLOPT_URL, addUrl.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postData.c_str());
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, cookieFile.c_str()); // Load cookies

    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        cerr << "Failed to add torrent: " << curl_easy_strerror(res) << "\n";
        curl_easy_cleanup(curl);
        return false;
    }

    curl_easy_cleanup(curl);
    return true;
}

bool checkMovieExist(const string& csvPath, const string& title, const string& year) {
    ifstream file(csvPath);
    if (!file.is_open()) {
        cerr << "Error opening CSV file: " << csvPath << endl;
        return false;  // Could not open file, assume movie does not exist
    }

    string line;
    string searchTitle = toLowerCase(title);
    string searchYear = year;

    while (getline(file, line)) {
        vector<string> parts;
        stringstream ss(line);
        string item;

        // Split CSV line into parts using comma
        while (getline(ss, item, ',')) {
            parts.push_back(item);
        }

        if (parts.size() >= 2) {
            string csvTitle = toLowerCase(parts[0]);
            string csvYear = parts[1];

            if (csvTitle == searchTitle && csvYear == searchYear) {
                return true;  // Found match
            }
        }
    }

    return false;  // No match
}

bool writeToCsv(const string& csvPath, const string& title, const string& year, const string& magnetLink) {

    // Extract the display name from magnet link
    string displayName;
    size_t pos = magnetLink.find("dn=");
    if (pos != string::npos) {
        size_t end = magnetLink.find("&", pos);
        string encodedName = magnetLink.substr(pos + 3, end - (pos + 3));
        replace(encodedName.begin(), encodedName.end(), '+', ' ');

        for (size_t i = 0; i < encodedName.length(); ++i) {
            if (encodedName[i] == '%' && i + 2 < encodedName.length()) {
                string hexStr = encodedName.substr(i + 1, 2);
                char decodedChar = static_cast<char>(strtol(hexStr.c_str(), nullptr, 16));
                displayName += decodedChar;
                i += 2;
            }
            else {
                displayName += encodedName[i];
            }
        }
    }
    else {
        displayName = "Unknown";
    }

    ofstream file(csvPath, ios::app);
    if (!file.is_open()) {
        cerr << "Error opening CSV file for writing: " << csvPath << endl;
        cout << "\n";
        return false;
    }

    file << toLowerCase(title) << "," << year << "," << displayName << "," << magnetLink << "\n";

    cerr << "[INFO] Saved to CSV: " << title << " | Year: " << year << " | Display Name: " << displayName << " | Magnet: " << magnetLink << endl;

    cout << "\n";
    return true;
}

bool sendVolumeCommand(int change) {
    wstring pipeName = L"\\\\.\\pipe\\mpv-pipe";

    HANDLE hPipe = CreateFileW(
        pipeName.c_str(),
        GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL
    );

    if (hPipe == INVALID_HANDLE_VALUE) {
        wcerr << L"[ERROR] Could not open MPV pipe. Error: " << GetLastError() << L"\n";
        return false;
    }

    string key = (change > 0) ? "volume_up" : "volume_down";
    string json = R"({"command": ["keypress", ")" + key + R"("]})" "\n";

    bool success = true;
    for (int i = 0; i < abs(change); ++i) {
        DWORD bytesWritten;
        if (!WriteFile(hPipe, json.c_str(), (DWORD)json.length(), &bytesWritten, NULL) || bytesWritten != json.length()) {
            wcerr << L"[ERROR] Failed to write to pipe. Error: " << GetLastError() << L"\n";
            success = false;
            break;
        }
    }

    CloseHandle(hPipe);
    return success;
}

bool sendMPVCommand(const std::wstring& pipeName, const std::string& command) {
    HANDLE hPipe = CreateFileW(pipeName.c_str(), GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (hPipe == INVALID_HANDLE_VALUE) {
        std::wcerr << L"[ERROR] Failed to open pipe: " << pipeName << std::endl;
        return false;
    }

    std::string cmdWithNewline = command + "\n";
    DWORD bytesWritten = 0;
    BOOL success = WriteFile(hPipe, cmdWithNewline.c_str(), static_cast<DWORD>(cmdWithNewline.size()), &bytesWritten, NULL);

    CloseHandle(hPipe);
    if (!success) {
        std::cerr << "[ERROR] Failed to write to pipe." << std::endl;
        return false;
    }
    return bytesWritten == cmdWithNewline.size();
}

bool isProcessRunning(const std::wstring& processName) {
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE)
        return false;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(PROCESSENTRY32W);

    if (Process32FirstW(hSnapshot, &pe)) {
        do {
            if (processName == pe.szExeFile) {
                CloseHandle(hSnapshot);
                return true;
            }
        } while (Process32NextW(hSnapshot, &pe));
    }
    CloseHandle(hSnapshot);
    return false;
}

bool startMPV(const std::wstring& mpvPath = L"YOUR_MPV_FULL_PATH",
    const std::wstring& pipeName = L"\\\\.\\pipe\\mpv-pipe")
{
    try {
        std::wstring cmdLine = mpvPath + L" --input-ipc-server=" + pipeName;

        wchar_t* cmdLineBuffer = _wcsdup(cmdLine.c_str());

        STARTUPINFOW si = { 0 };
        PROCESS_INFORMATION pi = { 0 };
        si.cb = sizeof(si);

        BOOL success = CreateProcessW(
            NULL,
            cmdLineBuffer,
            NULL,
            NULL,
            FALSE,
            0,
            NULL,
            NULL,
            &si,
            &pi
        );

        free(cmdLineBuffer);

        if (!success) {
            std::wcerr << L"[ERROR] Failed to launch MPV. Error Code: " << GetLastError() << std::endl;
            return false;
        }

        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "[EXCEPTION] startMPV crashed: " << e.what() << std::endl;
        return false;
    }
    catch (...) {
        std::cerr << "[EXCEPTION] startMPV crashed with unknown error." << std::endl;
        return false;
    }
}

inline std::string to_utf8(const std::filesystem::path& p) {
#if defined(_WIN32)
    auto u8 = p.u8string();
    std::string s(u8.begin(), u8.end());
    // Force forward slashes
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
#else
    return p.string();
#endif
}

bool playAndMonitorMovie(const std::filesystem::path& moviePath, bool isMovie) {

    try {
        std::wstring pipeName = L"\\\\.\\pipe\\mpv-pipe";
        std::wstring mpvPath = L"YOUR_MPV_FULL_PATH"; // use double backslashes as seperators
        std::filesystem::path fullPath = isMovie
            ? std::filesystem::path(L"YOUR_MOVIES_STORAGE_PATH") / moviePath
            : moviePath;

        // Start MPV if not running (idle mode, no file loaded)
        if (!isProcessRunning(L"mpv.exe")) {
            if (!startMPV(mpvPath, pipeName)) {
                std::wcerr << L"[ERROR] Failed to launch MPV (startMPV).\n";
                return false;
            }
            // Wait for IPC pipe to be available
            for (int i = 0; i < 15; ++i) {
                HANDLE hPipe = CreateFileW(
                    pipeName.c_str(), GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
                if (hPipe != INVALID_HANDLE_VALUE) {
                    CloseHandle(hPipe);
                    break;
                }
                Sleep(200);
            }
        }

        // Always use IPC to load the file
        std::string cmd = "{\"command\": [\"loadfile\", \"" + to_utf8(fullPath) + "\"]}";

        sendMPVCommand(pipeName, cmd);

        while (true) {
            // Only call once per loop!
            bool mpvRunning = isProcessRunning(L"mpv.exe");

            if (mpvRunning) {

                if ((isMovie && sleepModeActive) || (!isMovie && !sleepModeActive)) {
                    sendKeyToMPV(VK_ESCAPE);
                    std::wcout << L"[INFO] Mode switch detected during playback! ESC sent to MPV.\n";
                    return false;
                }


                if (skipRequested) {
                    skipRequested = false;
                    sendKeyToMPV(VK_ESCAPE);
                    std::wcout << L"[INFO] Skip requested. ESC sent to MPV.\n";
                    return false;
                }
                if (volumeUpRequested) {
                    volumeUpRequested = false;
                    bool ok = sendVolumeCommand(3);
                    std::wcout << L"[DEBUG] Sent volume up: " << (ok ? L"OK" : L"FAILED") << std::endl;
                }
                if (volumeDownRequested) {
                    volumeDownRequested = false;
                    bool ok = sendVolumeCommand(-3);
                    std::wcout << L"[DEBUG] Sent volume down: " << (ok ? L"OK" : L"FAILED") << std::endl;
                }
            }
            else {
                std::wcerr << L"[INFO] MPV process has closed.\n";
                return false;
            }

            Sleep(50);
        }
    }
    catch (const std::exception& e) {
        std::cerr << "[EXCEPTION] playAndMonitorMovie crashed: " << e.what() << std::endl;
        return true;
    }
    catch (...) {
        std::cerr << "[EXCEPTION] playAndMonitorMovie crashed with unknown error." << std::endl;
        return true;
    }
}

vector<fs::path> getAllMoviesInFolder(const fs::path& folderPath) {
    vector<fs::path> movies;

    try {
        // Search for all movie files
        for (const auto& entry : fs::recursive_directory_iterator(folderPath)) {
            if (entry.is_regular_file()) {
                fs::path filePath = entry.path();
                wstring ext_w = filePath.extension().wstring();

                // Convert extension to lowercase
                for (auto& ch : ext_w) ch = towlower(ch);

                if (ext_w == L".mp4" || ext_w == L".mkv" || ext_w == L".avi" || ext_w == L".mov" ||
                    ext_w == L".wmv" || ext_w == L".flv" || ext_w == L".mpeg" || ext_w == L".mpg" ||
                    ext_w == L".webm" || ext_w == L".vob" || ext_w == L".ts" || ext_w == L".m4v" ||
                    ext_w == L".3gp" || ext_w == L".rmvb") {

                    movies.push_back(filePath); // Add movie to list
                }
            }
        }
    }
    catch (const fs::filesystem_error& e) {
        wcerr << L"[ERROR] Filesystem error: " << e.what() << L"\n";
    }

    return movies; // Return all movie file paths
}

bool LaunchJackettInBackground(const std::string& path) {
    STARTUPINFOA si = { 0 };
    PROCESS_INFORMATION pi = { 0 };

    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE; // Hide the window

    BOOL success = CreateProcessA(
        NULL,
        (LPSTR)path.c_str(), // command line
        NULL, NULL, FALSE,
        CREATE_NO_WINDOW,
        NULL, NULL,
        &si,
        &pi
    );

    if (success) {
        // Close handles to prevent leaks, process runs independently
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return true;
    }
    else {
        cerr << "Failed to launch Jackett: " << GetLastError() << "\n";
        return false;
    }
}

bool downloadByTitle(const string& singleMovieTitle) {
    _setmode(_fileno(stdout), _O_TEXT);

    string apiKey = "JACKETT_API_KEY";
    string csvPath = "YOUR_DOWNLOADED_MOVIES_STORAGE_PATH";

    string singleSearchYear = extractYear(singleMovieTitle);
    string singleSearchYTitle = extractTitle(singleMovieTitle);

    if (apiKey.empty()) {
        cerr << "API key not set. Please update the API_KEY variable in the source code.\n";
        return false;
    }

    if (!LaunchJackettInBackground("YOUR_JACKETT_FULL_PATH")) {
        cerr << "Could not start Jackett.\n";
        return false;
    }

    if (singleMovieTitle.empty()) {
        return false;
    }

    string query;
    if (!startsWithDigits(singleSearchYear, 4)) {
        query = singleMovieTitle;
    }
    else {
        query = singleSearchYTitle + " " + singleSearchYear;
    }

    auto torrent = searchJackett(apiKey, query);

    // Score and sort torrents
    sort(torrent.begin(), torrent.end(), [&singleSearchYTitle](const TorrentResult& a, const TorrentResult& b) {
        return score_torrent(a, singleSearchYTitle) > score_torrent(b, singleSearchYTitle);
        });

    // Pick best valid torrent
    TorrentResult best;
    bool found = false;
    for (const auto& t : torrent) {
        if (score_torrent(t, singleSearchYTitle) > 0) {
            best = t;
            found = true;
            break;
        }
    }

    if (!found) {
        cerr << "[ERROR] No matching torrent found.\n";
        return false;
    }

    cout << "******* BEST TORRENT SELECTED *******\n";
    cout << "Found Torrents: " << torrent.size() << "\n";
    cout << "Title: " << best.title << "\n";
    cout << "Download: " << best.magnetLink << "\n";
    cout << "Seeders: " << best.seeders << ", Leechers: " << best.leechers << "\n";
    cout << fixed << setprecision(2);
    cout << "Size: " << bytesToGB(best.sizeBytes) << " GB\n\n";

    string qbHost = "YOUR_QBITTORRENT_WEB_UI_IP_PORT";  // qBittorrent WebUI IP and port
    string username = "YOUR_QBITTORRENT_USER_NAME";
    string password = "YOUR_QBITTORRENT_PASSWORD";
    string magnetLink = best.magnetLink;

    bool success = sendToQbittorrent(qbHost, username, password, magnetLink);

    if (!success) {
        cerr << "Failed to send magnet link.\n\n";
        return false;
    }

    bool csvWrite = writeToCsv(csvPath, toLowerCase(singleSearchYTitle), singleSearchYear, best.magnetLink);
    if (csvWrite) {
        cout << "[SUCCESS] Movie written to CSV: " << singleSearchYTitle << " (" << singleSearchYear << ")\n";
    }
    else {
        cout << "[INFO] Movie already exists: " << singleSearchYTitle << " (" << singleSearchYear << ")\n";
    }

    cout << "Magnet link sent successfully.\n\n";
    return true;
}

void download_automation() {

    _setmode(_fileno(stdout), _O_TEXT);

    string apiKey = "YOUR_JACKET_API_KEY";
    string csvPath = "YOUR_MOVIES_DOWNLOADED_CSV_PATH";

    if (sizeof(apiKey) <= 1 || string(apiKey) == "") {
        cerr << "API key not set. Please update the API_KEY variable in the source code.\n";
        return;
    }

    if (!LaunchJackettInBackground("YOUR_JACKETTS_FULL_PATH")) { // jackett.exe full path
        cerr << "Could not start Jackett.\n";
        return;
    }

    map<string, string> params;

    map<int, string> genre_map = {
    {28, "Action"}, {12, "Adventure"}, {16, "Animation"}, {35, "Comedy"}, {80, "Crime"},
    {99, "Documentary"}, {18, "Drama"}, {10751, "Family"}, {14, "Fantasy"}, {36, "History"},
    {27, "Horror"}, {10402, "Music"}, {9648, "Mystery"}, {10749, "Romance"},
    {878, "Science Fiction"}, {10770, "TV Movie"}, {53, "Thriller"}, {10752, "War"}, {37, "Western"}
    };

    string year = "";
    string movieStartDate = "";
    string movieFinishDate = "";
    string genre = "";
    string vote = "";
    string voteAverage = "";

    // Default values
    params["with_original_language"] = "en";
    params["include_adult"] = "true";
    params["sort_by"] = "popularity.desc";

    cout << "\n\n    Movie range start date (yy-mm-dd): ";
    getline(cin >> ws, movieStartDate);
    params["primary_release_date.gte"] = movieStartDate.c_str();
    system("cls");

    cout << "\n\n    Movie range finish date (yy-mm-dd): ";
    getline(cin >> ws, movieFinishDate);
    params["primary_release_date.lte"] = movieFinishDate.c_str();
    system("cls");

    cout << "\n\n";
    cout << left;
    cout << "    " << setw(18) << "Action" << setw(8) << "28"
        << setw(18) << "Family" << setw(8) << "10751"
        << setw(22) << "Science Fiction" << setw(8) << "878" << "\n";

    cout << "    " << setw(18) << "Adventure" << setw(8) << "12"
        << setw(18) << "Fantasy" << setw(8) << "14"
        << setw(22) << "TV Movie" << setw(8) << "10770" << "\n";

    cout << "    " << setw(18) << "Animation" << setw(8) << "16"
        << setw(18) << "History" << setw(8) << "36"
        << setw(22) << "Thriller" << setw(8) << "53" << "\n";

    cout << "    " << setw(18) << "Comedy" << setw(8) << "35"
        << setw(18) << "Horror" << setw(8) << "27"
        << setw(22) << "War" << setw(8) << "10752" << "\n";

    cout << "    " << setw(18) << "Crime" << setw(8) << "80"
        << setw(18) << "Music" << setw(8) << "10402"
        << setw(22) << "Western" << setw(8) << "37" << "\n";

    cout << "    " << setw(18) << "Documentary" << setw(8) << "99"
        << setw(18) << "Mystery" << setw(8) << "9648" << "\n";

    cout << "    " << setw(18) << "Drama" << setw(8) << "18"
        << setw(18) << "Romance" << setw(8) << "10749" << "\n";

    cout << "\n\n    Please enter the genre code (Multiple add a comma seperator): ";
    getline(cin >> ws, genre);
    params["with_genres"] = genre.c_str();
    system("cls");

    cout << "\n\n" << left;
    cout << "    " << setw(6) << "50" << "Loose filter (more results).\n";
    cout << "    " << setw(6) << "100" << "Balanced (used in most apps).\n";
    cout << "    " << setw(6) << "500" << "Stricter (only well-known movies).\n";
    cout << "    " << setw(6) << "1000" << "Very popular / high engagement only.\n";

    cout << "\n\n    Please enter the minimum vote count (100+): ";
    cin >> vote;

    params["vote_count.gte"] = vote.c_str();
    system("cls");

    cout << "\n\n    Please enter the minimum vote average (0.0 - 10.0): ";
    getline(cin >> ws, voteAverage);
    params["vote_average.gte"] = voteAverage.c_str();
    params["vote_average.lte"] = "10.0";
    system("cls");

    vector<json> movies = fetch_custom_movies(params);

    int counter = 0;

    for (const auto& movie : movies) {
        counter++;
        cout << "Title: " << movie.value("title", "N/A") << "\n";
        cout << "Genres: ";
        if (movie.contains("genre_ids") && movie["genre_ids"].is_array()) {
            for (const auto& gid : movie["genre_ids"]) {
                int id = gid.get<int>();
                cout << (genre_map.count(id) ? genre_map[id] : to_string(id)) << ", ";
            }
        }
        cout << "\n";
        cout << "Release Date: " << movie.value("release_date", "Unknown") << "\n";
        cout << "Language: " << movie.value("original_language", "Unknown") << "\n";
        cout << "Vote Average: " << movie.value("vote_average", 0.0) << "\n";
        cout << "Vote Count: " << movie.value("vote_count", 0) << "\n";
        cout << "Popularity: " << movie.value("popularity", 0.0) << "\n";
        cout << "Adult: " << (movie.value("adult", false) ? "Yes" : "No") << "\n";
        cout << "Video: " << (movie.value("video", false) ? "Yes" : "No") << "\n";
        cout << "Overview: " << movie.value("overview", "No overview") << "\n\n";

        cout << "Movie No: " << to_string(counter) << " of " << to_string(movies.size()) << "\n\n";

        string title = movie.value("title", "");
        string year = movie.value("release_date", "").substr(0, 4); // Assumes valid date string

        string movieQuery = title + " " + year;


        bool movieExist = checkMovieExist(csvPath, toLowerCase(title), year);

        if (movieExist) {
            cerr << "[INFO] Already downloaded: " << title << endl;
            cout << "\n";
            continue;
        }

        auto torrents = searchJackett(apiKey, movieQuery);

        // Score and sort all torrents
        sort(torrents.begin(), torrents.end(), [&title](const TorrentResult& a, const TorrentResult& b) {
            return score_torrent(a, title) > score_torrent(b, title);
            });

        // Select top valid torrent
        TorrentResult best;
        for (const auto& t : torrents) {
            if (score_torrent(t, title) > 0) {
                best = t;
                break;
            }
        }

        cout << "******* BEST TORRENT SELECTED *******\n";
        cout << "Found Torrents: " << torrents.size() << "\n";
        cout << "Title: " << best.title << "\n";
        cout << "Download: " << best.magnetLink << "\n";
        cout << "Seeders: " << best.seeders << ", Leechers: " << best.leechers << "\n";
        double sizeGB = bytesToGB(best.sizeBytes);
        cout << fixed << setprecision(2);
        cout << "Size: " << sizeGB << " GB" << endl;
        cout << "\n";

        string qbHost = "YOUR_QBITTORENT_WEB_UI_IP_PORT";  // Your qBittorrent WebUI IP and port
        string username = "YOUR_QBITTORENT_USERNAME";  // Default unless changed
        string password = "YOUR_QBITTORENT_PASSWORD";  // Your password
        string magnetLink = best.magnetLink; // Replace with real magnet

        bool res;
        bool success = sendToQbittorrent(qbHost, username, password, magnetLink);

        if (success) {
            res = writeToCsv(csvPath, toLowerCase(title), year, best.magnetLink);

            if (res) {
                cout << "[SUCCESS] Movie written to CSV: " << title << " (" << year << ")\n";
            }
            else {
                cout << "[INFO] Movie already exists: " << title << " (" << year << ")\n";
            }

            cout << "Magnet link sent successfully.\n\n";
        }
        else {
            cerr << "Failed to send magnet link.\n\n";
        }
    }

    return;
}

string getDateString(int daysAgo = 0) {
    time_t now = time(0) - (daysAgo * 86400);
    tm ltm;
    localtime_s(&ltm, &now);
    char buf[11];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d", 1900 + ltm.tm_year, 1 + ltm.tm_mon, ltm.tm_mday);
    return string(buf);
}

void download_full_automation() {
    _setmode(_fileno(stdout), _O_TEXT);

    string apiKey = "YOUR_JACKETT_API_KEY";
    string csvPath = "YOUR_DOWNLOADED_NOVIES_CSV_PATH";
    string qbHost = "YOUR_QBITTORRENT_HOST_PORT_ADDRESS";
    string username = "YOUR_QBITTORRENT_USER_NAME";
    string password = "YOUR_QBITTORRENT_PASSWORD";

    map<int, string> genre_map = {
        {28, "Action"}, {12, "Adventure"}, {16, "Animation"}, {35, "Comedy"}, {80, "Crime"},
        {99, "Documentary"}, {18, "Drama"}, {10751, "Family"}, {14, "Fantasy"}, {36, "History"},
        {27, "Horror"}, {10402, "Music"}, {9648, "Mystery"}, {10749, "Romance"},
        {878, "Science Fiction"}, {10770, "TV Movie"}, {53, "Thriller"}, {10752, "War"}, {37, "Western"}
    };

    // ** Persistent loop **
    while (true) {
        // Date range: last 3 months
        string movieFinishDate = getDateString(0);      // today
        string movieStartDate = getDateString(90);      // 90 days ago

        for (auto& [genre_id, genre_name] : genre_map) {
            map<string, string> params;
            params["with_original_language"] = "en";
            params["include_adult"] = "true";
            params["sort_by"] = "popularity.desc";
            params["primary_release_date.gte"] = movieStartDate;
            params["primary_release_date.lte"] = movieFinishDate;
            params["with_genres"] = std::to_string(genre_id);
            params["vote_count.gte"] = "100";
            params["vote_average.gte"] = "0";
            params["vote_average.lte"] = "10.0";

            vector<json> movies = fetch_custom_movies(params);

            for (const auto& movie : movies) {
                string title = movie.value("title", "");
                string year = movie.value("release_date", "").substr(0, 4);

                if (title.empty() || year.empty()) continue;

                if (checkMovieExist(csvPath, toLowerCase(title), year)) continue;

                string movieQuery = title + " " + year;
                auto torrents = searchJackett(apiKey, movieQuery);

                sort(torrents.begin(), torrents.end(), [&title](const TorrentResult& a, const TorrentResult& b) {
                    return score_torrent(a, title) > score_torrent(b, title);
                    });

                TorrentResult best;
                for (const auto& t : torrents) {
                    if (score_torrent(t, title) > 0) {
                        best = t;
                        break;
                    }
                }

                if (!best.magnetLink.empty()) {
                    if (sendToQbittorrent(qbHost, username, password, best.magnetLink)) {
                        writeToCsv(csvPath, toLowerCase(title), year, best.magnetLink);
                        cout << "[SUCCESS] " << title << " (" << year << ") sent to qBittorrent.\n";
                    }
                }
            }
        }
        // Sleep 6 hours before checking again
        std::this_thread::sleep_for(std::chrono::hours(6));
    }
}
