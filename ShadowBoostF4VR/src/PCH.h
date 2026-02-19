#pragma once

#define NOMMNOSOUND
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

// CommonLibF4 must be included first (before Windows.h)
#include "F4SE/F4SE.h"
#include "RE/Fallout.h"
#include <REL/Relocation.h>

// Windows headers (after CommonLib, needed for dynamic DLL loading)
#include <Windows.h>
#include <ShlObj.h>

using namespace std::literals;

// F4VRCommonFramework
#include "Logger.h"

using namespace f4cf;

#define DLLEXPORT __declspec(dllexport)

// Standard library
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// SimpleIni
#include <SimpleIni.h>
