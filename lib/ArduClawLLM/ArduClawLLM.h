#ifndef ARDUCLAW_LLM_H
#define ARDUCLAW_LLM_H

#include <Arduino.h>
#include <functional>

class ArduClawLLM {
public:
    ArduClawLLM();

    // Konfigurasi
    void setProvider(const String& provider);
    void setEndpoint(const String& endpoint);
    void setApiKey(const String& apiKey);
    void setModel(const String& model);
    void setMaxTokens(int maxTokens);
    void setTemperature(float temperature);

    String getProvider();
    String getEndpoint();
    String getApiKey();
    String getModel();
    int    getMaxTokens();
    float  getTemperature();

    // Chat completion - syncronous, blocking
    // Returns response text, or empty string on failure
    String chat(const String& userMessage);

    // Chat dengan system prompt
    String chat(const String& systemPrompt, const String& userMessage);

    // Non-blocking version via callback
    using ChatCallback = std::function<void(const String& response, bool success)>;
    void chatAsync(const String& userMessage, ChatCallback callback);

    // Status
    bool isConfigured();
    String getLastError();

private:
    String _provider;
    String _endpoint;
    String _apiKey;
    String _model;
    int    _maxTokens;
    float  _temperature;
    String _lastError;

    String buildRequestBody(const String& systemPrompt, const String& userMessage);
    String parseResponse(const String& jsonResponse);
    String httpPost(const String& url, const String& body);

    static String trimToNull(const String& str);
};

#endif
