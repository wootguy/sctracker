#include "util.h"
#include <curl/curl.h>
#include <fstream>
#include <chrono>

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