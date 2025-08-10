#include "NoLimitMovies.h"
#include <iostream>
#include <fstream>
#include <io.h> 
#include <fcntl.h>    
#include <Windows.h>
#include <filesystem>
#include <codecvt>
#include <locale>
#include <set>
#include <atomic>
std::atomic<bool> sleepModeActive = false;

namespace fs = std::filesystem;

using namespace std;

// External drive
const fs::path watchedCsvPath = L"YOUR_WATCHED_CSV_FULL_PATH";

wstring currentMoviePlaying;

string toUtf8(const fs::path& path) {
    u8string u8str = path.u8string();
    return string(u8str.begin(), u8str.end());
}

bool isMovieWatched(const fs::path& moviePath) {
    ifstream file(watchedCsvPath);
    if (!file.is_open()) return false;

    string utf8Movie = toUtf8(moviePath);
    string line;

    while (getline(file, line)) {
        if (line == utf8Movie) return true;
    }

    return false;
}

void logWatchedMovie(const fs::path& moviePath) {
    if (isMovieWatched(moviePath)) return;

    ofstream file(watchedCsvPath, ios::app);
    if (!file.is_open()) return;

    file << toUtf8(moviePath) << "\n";
}

void autoRunMovies() {

    _setmode(_fileno(stdout), _O_U16TEXT); // UTF-16 for wcout

    try {
        fs::path path = L"YOUR_MOVIES_PATH";
        vector<fs::path> movieList = getAllMoviesInFolder(path);

        for (const auto& movie : movieList) {

            if (sleepModeActive) {
                wcout << L"[INFO] Sleep mode activated! Stopping movie playback.\n";
                break;
            }

            if (!isMovieWatched(movie)) {
                currentMoviePlaying = movie;
                logWatchedMovie(movie);
                bool result = playAndMonitorMovie(movie, true);
                if (!result) {
                    wcout << L"[INFO] Playback ended or was skipped.\n";
                    // break; // If you want to stop on error/skip
                }
            }
        }
    }
    catch (const std::exception& e) {
        cerr << "[EXCEPTION] autoRunMovies crashed: " << e.what() << "\n";
    }
    catch (...) {
        cerr << "[EXCEPTION] autoRunMovies crashed with unknown error.\n";
    }
    _setmode(_fileno(stdout), _O_TEXT);  // Back to ANSI
}

string questions() {

    system("cls");

    string res = "";

    cout << "\n\n";
    cout << "    1. Auto Play Movies\n";
    cout << "    2. Auto Movie Download\n\n";

    cout << "    Please choice from the following: ";
    cin >> res;

    system("cls");

    return res;
}

int main(int argc, char* argv[])
{
    startHttpListener();

    // Check if autorun argument was passed
    string mode = "";

    if (argc > 1) {
        string arg1 = argv[1];
        if (arg1 == "autorun") {
            mode = "autoRunMode";
        }
    }

    if (mode == "autoRunMode") {
        // If autorun mode, immediately start playing movies in a loop
        while (true) {
            if (sleepModeActive) {
                handleSleepRoute();   // Loops random audio
            }
            else {
                autoRunMovies();      // Loops movies
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    else {
        // Normal menu mode
        while (true) {
            system("cls");
            string answer = questions();

            if (answer == "1") {
                while (true) {
                    autoRunMovies();
                }
            }
            else if (answer == "2") {
                download_automation();
            }
        }
    }

    return 0;
}