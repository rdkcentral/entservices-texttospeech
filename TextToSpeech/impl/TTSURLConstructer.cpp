/**
* If not stated otherwise in this file or this component's LICENSE
* file the following copyright and licenses apply:
*
* Copyright 2024 RDK Management
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
**/

#include "TTSURLConstructer.h"
#include <curl/curl.h>
#include <unistd.h>
#include <regex>

static size_t WriteCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

static const std::map<std::string, int> speechRateMap = {
    {"slow", 25},
    {"medium", 50},
    {"fast", 75},
    {"faster", 90},
    {"fastest", 100}
};

// Validate TTS endpoint URL to prevent SSRF (RDKEMW-24487)
static bool isValidTTSEndpoint(const std::string& url)
{
    if (url.empty())
        return false;

    // Reject file:// protocol
    if (url.find("file://") == 0)
        return false;

    // Reject loopback addresses
    std::regex loopbackRegex(R"(https?://(127\.|0x7f\.\.\.|localhost|\[::1\]))", std::regex::icase);
    if (std::regex_search(url, loopbackRegex))
        return false;

    // Reject private IP ranges (10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16)
    std::regex privateRegex(R"(https?://(10\.|172\.(1[6-9]|2[0-9]|3[0-1])\.|192\.168\.))", std::regex::icase);
    if (std::regex_search(url, privateRegex))
        return false;

    // Accept HTTPS endpoints
    if (url.find("https://") == 0)
        return true;

    // Reject HTTP endpoints (only HTTPS allowed for security)
    if (url.find("http://") == 0)
        return false;

    return false;
}

namespace TTS
{

TTSURLConstructer::TTSURLConstructer() { 

}

TTSURLConstructer::~TTSURLConstructer() {

}

std::string TTSURLConstructer::constructURL(TTSConfiguration &config, std::string text, bool isFallback, bool isLocal) {
    if(!(config.apiKey().empty()) && !isLocal && !(config.isRFCEnabled())) {
          TTSLOG_INFO("Device using remote sky endpoint");
          return httppostURL(config, text, isFallback);
     } else {
          TTSLOG_INFO("Device using %s endpoint", isLocal? "Local":"Remote");
          return httpgetURL(config, text, isFallback, isLocal);
     }
}

std::string TTSURLConstructer::httpgetURL(TTSConfiguration &config, std::string text, bool isfallback, bool isLocal) {
    // EndPoint URL
    std::string endPoint = isLocal ? config.localEndPoint() : (config.isRFCEnabled() ? config.rfcEndPoint() : config.secureEndPoint());

    // Validate endpoint to prevent SSRF (RDKEMW-24487)
    if (!isLocal && !isValidTTSEndpoint(endPoint)) {
        TTSLOG_ERROR("Invalid or unsafe TTS endpoint: %s", endPoint.c_str());
        return "";
    }

    std::string ttsRequest;
    ttsRequest.append(endPoint);

    // Voice
    if(!config.voice().empty()) {
        ttsRequest.append("voice=");
        ttsRequest.append(isLocal ? config.localVoice() : config.voice());
    }

    // Language
    if(!config.language().empty()) {
        ttsRequest.append("&language=");
        ttsRequest.append(config.language());
    }

    bool TTS1 = ((config.endPointType().compare("TTS2")) != 0);
    if(isLocal || TTS1) {
        ttsRequest.append("&rate=");
        auto it = speechRateMap.find(config.speechRate());
        ttsRequest.append(std::to_string((speechRateMap.end() != it ) ? it->second : 50));
    } else {
        //TTS 2.0
        ttsRequest.append("&speaking_rate=");
        ttsRequest.append(config.speechRate());
    }
    
    // Sanitize String
    std::string sanitizedString;
    sanitizeString((isfallback ? config.getFallbackValue() : text), sanitizedString);

    ttsRequest.append("&text=");
    ttsRequest.append(sanitizedString);
    return ttsRequest;
}

std::string  TTSURLConstructer::httppostURL(TTSConfiguration &config, std::string text, bool isFallback) {
    std::string ttsRequest;
    CURL *curl = curl_easy_init();

    // Validate endpoint to prevent SSRF (RDKEMW-24487)
    if (!isValidTTSEndpoint(config.secureEndPoint())) {
        TTSLOG_ERROR("Invalid or unsafe TTS endpoint: %s", config.secureEndPoint().c_str());
        return "";
    }

    if(curl) {
        CURLcode res;
        struct curl_slist *list = NULL;
        std::string readBuffer;
        JsonObject jsonConfig;
        JsonObject parameters;
        std::string post_data;

        if(isFallback) {
            jsonConfig["input"] = config.getFallbackValue();
        } else {
            jsonConfig["input"] = text;
        }

        jsonConfig["language"] = config.language();
        jsonConfig["voice"] = config.voice();
        jsonConfig["encoding"] = "mp3";
        jsonConfig.ToString(post_data);
        TTSLOG_INFO("gcd postdata :%s\n",post_data.c_str());

        res = curl_easy_setopt(curl, CURLOPT_URL,config.secureEndPoint().c_str()); //cid 280424
        if( res != CURLE_OK )
        {
             TTSLOG_ERROR("CURL error is:  %s\n", curl_easy_strerror(res));
        }
        res = curl_easy_setopt(curl, CURLOPT_POST, 1L);
        if( res != CURLE_OK )
        {
            TTSLOG_ERROR("CURL error is:  %s\n", curl_easy_strerror(res));
        }
        res = curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 2L);
         if( res != CURLE_OK )
        {
            TTSLOG_ERROR("CURL error is:  %s\n", curl_easy_strerror(res));
        }
        res = curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data.c_str());
         if( res != CURLE_OK )
        {
            TTSLOG_ERROR("CURL error is:  %s\n", curl_easy_strerror(res));
        }
        list = curl_slist_append(list, "content-type: application/json");
        list = curl_slist_append(list, (std::string("x-api-key: ") +
                                            config.apiKey()).c_str() );
        res = curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
         if( res != CURLE_OK )
        {
            TTSLOG_ERROR("CURL error is:  %s\n", curl_easy_strerror(res));
        }
        res = curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
         if( res != CURLE_OK )
        {
            TTSLOG_ERROR("CURL error is:  %s\n", curl_easy_strerror(res));
        }
        res = curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
         if( res != CURLE_OK )
        {
            TTSLOG_ERROR("CURL error is:  %s\n", curl_easy_strerror(res));
        }

        res = curl_easy_perform(curl);
        if ( res != CURLE_OK ) {
            TTSLOG_ERROR("TTS: Error in interacting with endpoint. CURL error is:  %s\n", curl_easy_strerror(res));
            if( config.isFallbackEnabled() && isFallback == false) {
                TTSLOG_INFO("RDK TTS: Device is not connected with Internet, hence speaking fallback text in place of actual text: %s",text.c_str());
                ttsRequest.assign(config.getFallbackPath());
            } 
        } else {
            TTSLOG_INFO("gcd curl response :%s\n",readBuffer.c_str());
            parameters.FromString(readBuffer);
            ttsRequest.assign(parameters["url"].String());
        }
        curl_slist_free_all(list);
        curl_easy_cleanup(curl);
    }
    return ttsRequest;
}

void TTSURLConstructer::replaceIfIsolated(std::string& text, const std::string& search, const std::string& replace, bool skipIsolationCheck) {
    size_t pos = 0;
    while ((pos = text.find(search, pos)) != std::string::npos) {
        bool punctBefore = (pos == 0 || std::ispunct(text[pos-1]) || std::isspace(text[pos-1]));
        bool punctAfter = (pos+1 == text.length() || std::ispunct(text[pos+1]) || std::isspace(text[pos+1]));

        if((punctBefore && punctAfter) || skipIsolationCheck) {
            text.replace(pos, search.length(), replace);
            pos += replace.length();
        } else {
            pos += search.length();
        }
    }
}

bool TTSURLConstructer::isSilentPunctuation(const char c) {
    static std::string SilentPunctuation = "?!:;-()";
    return (SilentPunctuation.find(c) != std::string::npos);
}

void TTSURLConstructer::replaceSuccesivePunctuation(std::string& text) {
    size_t pos = 0;
    while(pos < text.length()) {
        // Remove unwanted characters
        static std::string stray = "\"";
        if(stray.find(text[pos]) != std::string::npos) {
            text.erase(pos,1);
            if(++pos == text.length())
                break;
        }

        if(ispunct(text[pos])) {
            ++pos;
            while(pos < text.length() && (isSilentPunctuation(text[pos]) || isspace(text[pos]))) {
                if(isSilentPunctuation(text[pos]))
                    text.erase(pos,1);
                else
                    ++pos;
            }
        } else {
            ++pos;
        }
    }
}

void TTSURLConstructer::curlSanitize(std::string &sanitizedString) {
    CURL *curl = curl_easy_init();
    if(curl) {
      char *output = curl_easy_escape(curl, sanitizedString.c_str(), sanitizedString.size());
      if(output) {
          sanitizedString = output;
          curl_free(output);
      }
    }
    curl_easy_cleanup(curl);
}

void TTSURLConstructer::sanitizeString(const std::string &input, std::string &sanitizedString) {
    sanitizedString = input;

    replaceIfIsolated(sanitizedString, "://", " colon slash slash ", true);
    replaceIfIsolated(sanitizedString, "$", "dollar");
    replaceIfIsolated(sanitizedString, "#", "pound");
    replaceIfIsolated(sanitizedString, "&", "and");
    replaceIfIsolated(sanitizedString, "|", "bar");
    replaceIfIsolated(sanitizedString, "/", "or");

    replaceSuccesivePunctuation(sanitizedString);

    curlSanitize(sanitizedString);

    TTSLOG_VERBOSE("In:%s, Out:%s", input.c_str(), sanitizedString.c_str());
}

}//namespace tts end
