#include "util.h"
#include <curl/curl.h>
#include <fstream>
#include <chrono>

#if defined(WIN32) || defined(_WIN32)
#include <Windows.h>
#include <direct.h>
#define GetCurrentDir _getcwd
#else
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#define GetCurrentDir getcwd

typedef char TCHAR;

void OutputDebugString(const char* str) {}
#endif

using namespace std::chrono;

vector<string> splitString(string str, const char* delimitters)
{
	vector<string> split;
	size_t start = 0;
	size_t end = str.find_first_of(delimitters);

	while (end != std::string::npos)
	{
		split.push_back(str.substr(start, end - start));
		start = end + 1;
		end = str.find_first_of(delimitters, start);
	}

	split.push_back(str.substr(start));

	return split;
}

string replaceString(string subject, string search, string replace) {
	size_t pos = 0;
	while ((pos = subject.find(search, pos)) != string::npos)
	{
		subject.replace(pos, search.length(), replace);
		pos += replace.length();
	}
	return subject;
}

bool fileExists(string path) {
	if (FILE* file = fopen(path.c_str(), "r"))
	{
		fclose(file);
		return true;
	}
	return false;
}

size_t writeFunction(void* ptr, size_t size, size_t nmemb, std::string* data) {
	data->append((char*)ptr, size * nmemb);
	return size * nmemb;
}

int webRequest(string url, string& response_string) {
	auto curl = curl_easy_init();
	response_string = "";

	if (curl) {
		curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
		curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
		curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 50L);
		curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);

		//std::string header_string;
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeFunction);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);
		//curl_easy_setopt(curl, CURLOPT_HEADERDATA, &header_string);

		curl_easy_perform(curl);

		long response_code;
		double elapsed;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
		curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &elapsed);

		curl_easy_cleanup(curl);
		curl = NULL;

		return response_code;
	}
}

char* loadFile(const string& fileName, int& length)
{
	if (!fileExists(fileName))
		return NULL;
	ifstream fin(fileName.c_str(), ifstream::in | ios::binary);
	long long begin = fin.tellg();
	fin.seekg(0, ios::end);
	uint32_t size = (uint32_t)((int)fin.tellg() - begin);
	char* buffer = new char[size + 1];
	fin.seekg(0);
	fin.read(buffer, size);
	fin.close();
	length = (int)size; // surely models will never exceed 2 GB
	buffer[size] = 0;
	return buffer;
}

uint64_t getEpochMillis() {
	return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

uint32_t getEpochSeconds() {
	return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

void winPath(string& path)
{
	for (int i = 0, size = path.size(); i < size; i++)
	{
		if (path[i] == '/')
			path[i] = '\\';
	}
}

vector<string> getDirFiles(string path, string extension, string startswith)
{
	vector<string> results;

#if defined(WIN32) || defined(_WIN32)
	path = path + startswith + "*." + extension;
	winPath(path);
	WIN32_FIND_DATA FindFileData;
	HANDLE hFind;

	//println("Target file is " + path);
	hFind = FindFirstFile(path.c_str(), &FindFileData);
	if (hFind == INVALID_HANDLE_VALUE)
	{
		//println("FindFirstFile failed " + str((int)GetLastError()) + " " + path);
		return results;
	}
	else
	{
		results.push_back(FindFileData.cFileName);

		while (FindNextFile(hFind, &FindFileData) != 0)
		{
			results.push_back(FindFileData.cFileName);
		}

		FindClose(hFind);
	}
#else
	extension = toLowerCase(extension);
	startswith = toLowerCase(startswith);
	startswith.erase(std::remove(startswith.begin(), startswith.end(), '*'), startswith.end());
	DIR* dir = opendir(path.c_str());

	if (!dir)
		return results;

	while (true)
	{
		dirent* entry = readdir(dir);

		if (!entry)
			break;

		if (entry->d_type == DT_DIR)
			continue;

		string name = string(entry->d_name);
		string lowerName = toLowerCase(name);

		if (extension.size() > name.size() || startswith.size() > name.size())
			continue;

		if (extension == "*" || std::equal(extension.rbegin(), extension.rend(), lowerName.rbegin()))
		{
			if (startswith.size() == 0 || std::equal(startswith.begin(), startswith.end(), lowerName.begin()))
			{
				results.push_back(name);
			}
		}
	}

	closedir(dir);
#endif

	return results;
}

bool dirExists(const string& path)
{
	struct stat info;

	if (stat(path.c_str(), &info) != 0)
		return false;
	else if (info.st_mode & S_IFDIR)
		return true;
	return false;
}