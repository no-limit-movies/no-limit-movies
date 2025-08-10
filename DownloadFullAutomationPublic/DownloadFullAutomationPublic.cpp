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
#include <fstream>
#include <unordered_set>
#include <Windows.h>
#include <filesystem>
#include <fcntl.h>
#include <io.h>
#include <ctime>

using namespace std;
using json = nlohmann::json;

const char API_KEY[] = "YOUR_TMDB_API_KEY";

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

bool contains_case(const string& str, const string& keyword) {
    auto it = search(str.begin(), str.end(),
        keyword.begin(), keyword.end(),
        [](char ch1, char ch2) { return tolower(ch1) == tolower(ch2); });
    return it != str.end();
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

string toLowerCase(const string& input) {
    string result = input;
    transform(result.begin(), result.end(), result.begin(),
        [](unsigned char c) { return tolower(c); });
    return result;
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

string getDateString(int daysAgo = 0) {
    time_t now = time(0) - (daysAgo * 86400);
    tm ltm;
    localtime_s(&ltm, &now);
    char buf[9]; // 8 chars + null-terminator
    snprintf(buf, sizeof(buf), "%02d-%02d-%02d", (ltm.tm_year + 1900) % 100, ltm.tm_mon + 1, ltm.tm_mday);
    return string(buf);
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

int main() {

    if (!LaunchJackettInBackground("YOUR_JACKETT_FULL_PATH")) {
        cerr << "Could not start Jackett.\n";
        return -1;
    }

    _setmode(_fileno(stdout), _O_TEXT);

    string apiKey = "YOUR_JACKET_API_KEY";
    string csvPath = "YOUR_DOWNLOADED_MOVIES_CSV_FULL_PATH";
    string qbHost = "YOUR_QBITTORRENT_HOST_IP_PORT";
    string username = "YOUR_QBITTORRENT_USERNAME";
    string password = "YOUR_QBITTORRENT_PASSWORD";

    map<int, string> genre_map = {
        {28, "Action"}, {12, "Adventure"}, {16, "Animation"}, {35, "Comedy"}, {80, "Crime"},
        {99, "Documentary"}, {18, "Drama"}, {10751, "Family"}, {14, "Fantasy"}, {36, "History"},
        {27, "Horror"}, {10402, "Music"}, {9648, "Mystery"}, {10749, "Romance"},
        {878, "Science Fiction"}, {10770, "TV Movie"}, {53, "Thriller"}, {10752, "War"}, {37, "Western"}
    };

    // ** Persistent loop **
    while (true) {
        string movieFinishDate = getDateString(0);      // today
        string movieStartDate = getDateString(90);      // 90 days ago

        cout << "\n================== NEW RUN: " << getDateString(0) << " ==================\n";
        cout << "Scraping movies from: " << movieStartDate << " to " << movieFinishDate << "\n";

        for (auto& [genre_id, genre_name] : genre_map) {
            cout << "\n------------------ GENRE: " << genre_name << " (" << genre_id << ") ------------------\n";
            map<string, string> params;
            params["with_original_language"] = "en";
            params["include_adult"] = "true";
            params["sort_by"] = "popularity.desc";
            params["primary_release_date.gte"] = movieStartDate;
            params["primary_release_date.lte"] = movieFinishDate;
            params["with_genres"] = std::to_string(genre_id);
            params["vote_count.gte"] = "100";
            params["vote_average.gte"] = "2.0";
            params["vote_average.lte"] = "10.0";

            cout << "[TMDb Search] Params: Genre=" << genre_name
                << ", Date Range=" << movieStartDate << " to " << movieFinishDate
                << ", Vote Count>=100, Vote Avg=3.0-10.0\n";

            vector<json> movies = fetch_custom_movies(params);

            cout << "[TMDb] Movies found: " << movies.size() << endl;

            for (const auto& movie : movies) {
                string title = movie.value("title", "");
                string year = movie.value("release_date", "").substr(0, 4);
                double voteAverage = movie.value("vote_average", 0.0);
                int voteCount = movie.value("vote_count", 0);
                double popularity = movie.value("popularity", 0.0);

                cout << "\n  - Movie: " << title << " (" << year << ")"
                    << " | Votes: " << voteCount
                    << " | Avg: " << voteAverage
                    << " | Pop: " << popularity;

                if (title.empty() || year.empty()) {
                    cout << "  [SKIP: No title or year]" << endl;
                    continue;
                }
                if (checkMovieExist(csvPath, toLowerCase(title), year)) {
                    cout << "  [SKIP: Already in CSV]" << endl;
                    continue;
                }

                string movieQuery = title + " " + year;
                cout << "\n    [Jackett] Searching for: " << movieQuery << endl;
                auto torrents = searchJackett(apiKey, movieQuery);

                cout << "    [Jackett] Results found: " << torrents.size() << endl;
                // Show first 2 best torrents for info
                sort(torrents.begin(), torrents.end(), [&title](const TorrentResult& a, const TorrentResult& b) {
                    return score_torrent(a, title) > score_torrent(b, title);
                    });

                for (int i = 0; i < min(2, (int)torrents.size()); ++i) {
                    const auto& t = torrents[i];
                    cout << "      [" << i + 1 << "] " << t.title
                        << " | Seeders: " << t.seeders
                        << " | Size: " << fixed << setprecision(2)
                        << (t.sizeBytes / (1024.0 * 1024 * 1024)) << " GB" << endl;
                }

                TorrentResult best;
                for (const auto& t : torrents) {
                    if (score_torrent(t, title) > 0) {
                        best = t;
                        break;
                    }
                }

                if (!best.magnetLink.empty()) {
                    cout << "    [SEND] To qBittorrent: " << best.title
                        << " | Seeders: " << best.seeders
                        << " | Size: " << fixed << setprecision(2)
                        << (best.sizeBytes / (1024.0 * 1024 * 1024)) << " GB"
                        << "\n    Magnet: " << best.magnetLink.substr(0, 70) << "..." << endl;

                    if (sendToQbittorrent(qbHost, username, password, best.magnetLink)) {
                        writeToCsv(csvPath, toLowerCase(title), year, best.magnetLink);
                        cout << "    [SUCCESS] " << title << " (" << year << ") sent to qBittorrent." << endl;
                    }
                    else {
                        cout << "    [FAILED] Could not send to qBittorrent." << endl;
                    }
                }
                else {
                    cout << "    [NO SUITABLE TORRENT FOUND]" << endl;
                }
            }
        }

        int sleepTime = 5;
        cout << "\n[INFO] Run complete. Sleeping for " + to_string(sleepTime) + " minute...\n";
        std::this_thread::sleep_for(std::chrono::minutes(sleepTime));
    }

    return 0;
}


