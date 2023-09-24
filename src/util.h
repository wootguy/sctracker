#pragma once
#include <string>
#include <vector>
#include <stdint.h>

using namespace std;

int webRequest(string url, string& response_string);

vector<string> splitString(string str, const char* delimitters);

string replaceString(string subject, string search, string replace);

bool fileExists(string path);

char* loadFile(const string& fileName, int& length);

uint64_t getEpochMillis();

uint32_t getEpochSeconds();

vector<string> getDirFiles(string path, string extension, string startswith);

bool dirExists(const string& path);

bool createDir(const string& path);