#pragma once
#include "include/cef_values.h"
#include "include/cef_parser.h"
#include "include/internal/cef_types.h"
#include <string>
#include <filesystem>
#include <fstream>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include <shlobj.h>
#endif

//Parses the save files of nuts and bolts and converts it to a json that the website can read for the model viewer.

namespace detail {
#ifdef _WIN32
	inline uint16_t bswap16(uint16_t v) { return _byteswap_ushort(v); }
	inline uint32_t bswap32(uint32_t v) { return _byteswap_ulong(v); }
#else
	inline uint16_t bswap16(uint16_t v) { return static_cast<uint16_t>((v >> 8) | (v << 8)); }
	inline uint32_t bswap32(uint32_t v) {
		return ((v & 0xFF000000u) >> 24) | ((v & 0x00FF0000u) >> 8)
		     | ((v & 0x0000FF00u) << 8)  | ((v & 0x000000FFu) << 24);
	}
#endif
	inline float bswapf(float v) {
		uint32_t tmp;
		memcpy(&tmp, &v, 4);
		tmp = bswap32(tmp);
		memcpy(&v, &tmp, 4);
		return v;
	}
	template<typename T>
	inline T bread(std::ifstream& f) {
		T val{};
		f.read(reinterpret_cast<char*>(&val), sizeof(T));
		return val;
	}
	inline uint16_t bread16(std::ifstream& f) { return bswap16(bread<uint16_t>(f)); }
	inline uint32_t bread32(std::ifstream& f) { return bswap32(bread<uint32_t>(f)); }
	inline int32_t  breadi32(std::ifstream& f) { return static_cast<int32_t>(bswap32(bread<uint32_t>(f))); }
	inline float    breadf(std::ifstream& f)   { return bswapf(bread<float>(f)); }
}

struct vec3 {
    float x, y, z;
};

struct vehiclePart {
    int ID; //Used to know what shape to use
    vec3 position;
    vec3 rot; //ypr
    unsigned int color;
    bool isPainted;
};

struct Vehicle {
	unsigned short numOfParts;
	// A buffer of 0x40 bytes is reserved for this.
	// A total of 32 characters will be in this.
	wchar_t vehicleUnicodeName[0x20];

    //Contains each part of the vehicle
	std::vector<vehiclePart> parts;
};

inline std::string WStringToUtf8(const wchar_t* wstr) {
    if (!wstr || wstr[0] == L'\0')
        return {};
#ifdef _WIN32
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, NULL, 0, NULL, NULL);
    if (size_needed <= 0)
        return {};
    std::string result(size_needed - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &result[0], size_needed, NULL, NULL);
    return result;
#else
    // On Linux wchar_t is 4 bytes (UTF-32); encode each code point to UTF-8.
    std::string result;
    for (const wchar_t* p = wstr; *p; ++p) {
        uint32_t cp = static_cast<uint32_t>(*p);
        if (cp < 0x80) {
            result += static_cast<char>(cp);
        } else if (cp < 0x800) {
            result += static_cast<char>(0xC0 | (cp >> 6));
            result += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            result += static_cast<char>(0xE0 | (cp >> 12));
            result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            result += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            result += static_cast<char>(0xF0 | (cp >> 18));
            result += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            result += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return result;
#endif
}


CefRefPtr<CefValue> SerializeVehicle(const Vehicle& v) {
    auto root = CefDictionaryValue::Create();
	root->SetString("name", WStringToUtf8(v.vehicleUnicodeName));

    auto parts = CefListValue::Create();
	for (int i = 0; i < (int)v.parts.size(); i++) {
		const vehiclePart& p = v.parts[i];
		auto part = CefDictionaryValue::Create();
		part->SetInt("shapeId", p.ID);
		part->SetDouble("px", p.position.x); part->SetDouble("py", p.position.y); part->SetDouble("pz", p.position.z);
		part->SetDouble("rx", p.rot.x);      part->SetDouble("ry", p.rot.y);      part->SetDouble("rz", p.rot.z);
		part->SetInt("color", (int)p.color);
		part->SetBool("isPainted", p.isPainted);
		parts->SetDictionary(i, part);
	}
    root->SetList("parts", parts);
    
    auto value = CefValue::Create();
    value->SetDictionary(root);
    return value;
}

class VehicleSaveManager {
    public:
		std::vector< Vehicle> vehicles;
		void ReloadVehicles() {
			vehicles.clear();

#ifdef _WIN32
			wchar_t docsPathW[MAX_PATH];
			if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, docsPathW)))
				return;
			std::filesystem::path basePath = std::filesystem::path(docsPathW) / "renut" / "B13EBABEBABEBABE" / "4D5307ED";
#else
			const char* home = getenv("HOME");
			if (!home) return;
			std::filesystem::path basePath = std::filesystem::path(home) / "renut" / "B13EBABEBABEBABE" / "4D5307ED";
#endif
			std::filesystem::path headersPath = basePath / "Headers" / "00000001";

			if (!std::filesystem::exists(headersPath))
				return;

			static const uint8_t vehicleMagic[] = {
				0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
				0x00, 0x56, 0x00, 0x45, 0x00, 0x48, 0x00, 0x49,
				0x00, 0x43, 0x00, 0x4C, 0x00, 0x45, 0x00, 0x3A
			};

			for (const auto& entry : std::filesystem::directory_iterator(headersPath)) {
				if (entry.path().extension() != ".header") continue;

				std::ifstream headerFile(entry.path(), std::ios::binary);
				if (!headerFile) continue;

				uint8_t magic[24];
				headerFile.read(reinterpret_cast<char*>(magic), 24);
				if (headerFile.gcount() < 24 || memcmp(magic, vehicleMagic, 24) != 0) continue;

				// Read unicode vehicle name at byte 26, stored as big-endian UTF-16
				headerFile.seekg(26);
				wchar_t unicodeName[0x20] = {};
				for (int i = 0; i < 0x20; i++) {
					uint8_t hi, lo;
					if (!headerFile.read(reinterpret_cast<char*>(&hi), 1)) break;
					if (!headerFile.read(reinterpret_cast<char*>(&lo), 1)) break;
					uint16_t ch = static_cast<uint16_t>((hi << 8) | lo);
					if (ch == 0) break;
					unicodeName[i] = static_cast<wchar_t>(ch);
				}

				// Data file path: basePath/00000001/<stem>/<stem_without_0x_prefix>
				std::string stemStr = entry.path().stem().string();
				std::string fileId = (stemStr.size() > 2 && stemStr[0] == '0' && (stemStr[1] == 'x' || stemStr[1] == 'X'))
					? stemStr.substr(2) : stemStr;

				std::filesystem::path dataFilePath = basePath / "00000001" / stemStr / fileId;

				std::ifstream dataFile(dataFilePath, std::ios::binary);
				if (!dataFile) continue;

				// File layout (offsets from file start):
				// 0x00: checksum[8], 0x08: numOfParts, 0x0A: unk1, 0x0C-0x24: preload floats + unk,
				// 0x28: vehicleName[0x40], 0x68+: button assignments, 0x84: VehiclePart[]
				dataFile.seekg(0x08);
				unsigned short numOfParts = detail::bread16(dataFile);

				dataFile.seekg(0x28);
				wchar_t vehicleNameBuf[0x20] = {};
				for (int i = 0; i < 0x20; i++) {
					uint8_t hi, lo;
					if (!dataFile.read(reinterpret_cast<char*>(&hi), 1)) break;
					if (!dataFile.read(reinterpret_cast<char*>(&lo), 1)) break;
					uint16_t ch = static_cast<uint16_t>((hi << 8) | lo);
					if (ch == 0) break;
					vehicleNameBuf[i] = static_cast<wchar_t>(ch);
				}

				Vehicle v = {};
				memcpy(v.vehicleUnicodeName, vehicleNameBuf, sizeof(vehicleNameBuf));
				v.numOfParts = numOfParts;

				dataFile.seekg(0x84);
				for (int i = 0; i < numOfParts; i++) {
					int8_t xPos, yPos, zPos, isChallengePart, isPainted, unk1, unk2, unk3;
					uint32_t partIdx;
					float yaw, pitch, roll;
					uint32_t color;
					int32_t unk4, unk5;

					dataFile.read(reinterpret_cast<char*>(&xPos), 1);
					dataFile.read(reinterpret_cast<char*>(&yPos), 1);
					dataFile.read(reinterpret_cast<char*>(&zPos), 1);
					dataFile.read(reinterpret_cast<char*>(&isChallengePart), 1);
					dataFile.read(reinterpret_cast<char*>(&isPainted), 1);
					dataFile.read(reinterpret_cast<char*>(&unk1), 1);
					dataFile.read(reinterpret_cast<char*>(&unk2), 1);
					dataFile.read(reinterpret_cast<char*>(&unk3), 1);
					partIdx = detail::bread32(dataFile);
					yaw     = detail::breadf(dataFile);
					pitch   = detail::breadf(dataFile);
					roll    = detail::breadf(dataFile);
					color   = detail::bread32(dataFile);
					unk4    = detail::breadi32(dataFile);
					unk5    = detail::breadi32(dataFile);

					if (dataFile.fail()) break;

					vehiclePart part = {};
					part.ID = static_cast<int>(partIdx);
					part.position = { static_cast<float>(xPos), static_cast<float>(yPos), static_cast<float>(zPos) };
					part.rot = { yaw, pitch, roll };
					part.color = color;
					part.isPainted = (isPainted != 0);
					v.parts.push_back(part);
				}

				vehicles.push_back(v);
			}
		}

		void DumpAllVehiclesAsJson(const std::string& outputPath) {
			std::filesystem::create_directories(outputPath);

			for (const Vehicle& v : vehicles) {
				auto rootValue = SerializeVehicle(v);

				CefString json = CefWriteJSON(rootValue, JSON_WRITER_DEFAULT);

				std::string fileName = WStringToUtf8(v.vehicleUnicodeName) + ".json";
				std::ofstream out(std::filesystem::path(outputPath) / fileName);
				out << json.ToString();
			}
		}
};