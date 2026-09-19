#include "stdafx.h"
#include "NetUtils.h"
#include <Wininet.h>

#pragma comment(lib, "Wininet.lib")

// reply of the requery
UPEXPORT_CFUNC(size_t) req_reply(void *ptr, size_t size, size_t nmemb, void *stream)
{
	string *str = (string*)stream;
	(*str).append((char*)ptr, size*nmemb);
	return size * nmemb;
}

// http GET
UPEXPORT_CFUNC(CURLcode) curl_get_req(const string &url, string &response)
{
	response.clear();
	// Follow the Windows system proxy/PAC settings used by the browser.
	HINTERNET session = InternetOpenA("DzjsTrainer/1.0", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
	if (!session) return CURLE_FAILED_INIT;
	const DWORD timeout = 5000;
	InternetSetOptionA(session, INTERNET_OPTION_CONNECT_TIMEOUT, (LPVOID)&timeout, sizeof(timeout));
	InternetSetOptionA(session, INTERNET_OPTION_RECEIVE_TIMEOUT, (LPVOID)&timeout, sizeof(timeout));

	// Try the randomized wildcard host first. A transient WinINet/DNS failure
	// is retried once, then the apex host is used as a stable fallback.
	string candidates[3] = { url, url, url };
	const size_t scheme = url.find("https://");
	const size_t firstDot = scheme == string::npos ? string::npos : url.find('.', scheme + 8);
	const size_t baseDot = firstDot == string::npos ? string::npos : url.find('.', firstDot + 1);
	if (baseDot != string::npos && firstDot != string::npos)
		candidates[2] = url.substr(0, scheme + 8) + url.substr(firstDot + 1);
	for (const string& candidate : candidates) {
		HINTERNET request = InternetOpenUrlA(session, candidate.c_str(), nullptr, 0,
			INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_SECURE |
			INTERNET_FLAG_IGNORE_CERT_CN_INVALID | INTERNET_FLAG_IGNORE_CERT_DATE_INVALID, 0);
		if (!request) continue;
		response.clear();
		char buffer[8192]; DWORD read = 0;
		while (InternetReadFile(request, buffer, sizeof(buffer), &read) && read) response.append(buffer, read);
		DWORD status = 0, statusSize = sizeof(status);
		HttpQueryInfoA(request, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &statusSize, nullptr);
		InternetCloseHandle(request);
		if (status >= 200 && status < 400) { InternetCloseHandle(session); return CURLE_OK; }
	}
	InternetCloseHandle(session);
	return CURLE_COULDNT_CONNECT;
}

// http POST
UPEXPORT_CFUNC(CURLcode) curl_post_req(const string &url, const string &postParams, string &response)
{
	// init curl
	CURL *curl = curl_easy_init();
	// res code
	CURLcode res;
	if (curl)
	{
		// set params
		curl_easy_setopt(curl, CURLOPT_POST, 1); // post req
		curl_easy_setopt(curl, CURLOPT_URL, url.c_str()); // url
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postParams.c_str()); // params
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, false); // if want to use https
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, false); // set peer and host verify false
		curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);
		curl_easy_setopt(curl, CURLOPT_READFUNCTION, NULL);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, req_reply);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&response);
		curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);
		curl_easy_setopt(curl, CURLOPT_HEADER, 0);
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3);
		// start req
		res = curl_easy_perform(curl);
	}
	// release curl
	curl_easy_cleanup(curl);
	return res;
}
