#pragma once
#include <string>
#include <vector>
#include <stdint.h>
#include "rapidjson/document.h"

using namespace std;
using namespace rapidjson;

int webRequest(string url, string& response_string);

vector<string> splitString(string str, const char* delimitters);

string replaceString(string subject, string search, string replace);

bool fileExists(string path);

char* loadFile(const string& fileName, int& length);

string stringifyJson(Value& v);

bool writeJson(string path, Value& jsonVal);

uint64_t getEpochMillis();

uint32_t getEpochSeconds();

vector<string> getDirFiles(string path, string extension, string startswith);

bool dirExists(const string& path);

bool createDir(const string& path);

bool loadJson(string path, Document& doc);