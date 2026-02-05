#pragma once

#include <string>

// Submit a prompt to Ollama and return the concatenated response text.
std::string QueryOllamaLLM(const std::string& model, const std::string& prompt);
