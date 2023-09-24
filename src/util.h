#pragma once
#include <string>
#include <vector>
#include <stdint.h>

using namespace std;

int webRequest(string url, string& response_string);

vector<string> splitString(string str, const char* delimitters);

bool fileExists(string path);

char* loadFile(const string& fileName, int& length);

uint64_t getEpochMillis();