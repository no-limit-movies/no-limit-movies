#pragma once

#include <string>
#include <filesystem> // required for fs::path
#include "httplib.h"
#include <curl/curl.h>
#include <atomic>

extern std::atomic<bool> sleepModeActive;


namespace fs = std::filesystem;
using namespace std;

// DO NOT use `using namespace std;` in headers. It causes global namespace pollution.

extern wstring currentMoviePlaying;

void download_automation();
void handleSleepRoute();
bool downloadByTitle(const string& singleMovieTitle);
vector<fs::path> getAllMoviesInFolder(const fs::path& folderPath);
bool playAndMonitorMovie(const fs::path& moviePath, bool isMovie);
bool isSafeToDelete(const fs::path& folderToDelete);
void startHttpListener();
void download_full_automation();
