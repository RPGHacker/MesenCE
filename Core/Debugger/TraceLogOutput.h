#pragma once
#include "pch.h"

#include "Core/Shared/MessageManager.h"
#include "Utilities/CompressionHelper.h"

#include <memory>
#include <thread>
#include <mutex>
#include <queue>
#include <atomic>

class TraceLogOutput
{
public:
	enum CompressionType : uint8_t
	{
		None = 0,
		DiztinguishZlib = 1,
	};

	enum OutputFormat : uint8_t
	{
		Text = 0,
		Diztinguish = 1,
		DiztinguishAbridged = 2,

	};

	struct Settings
	{
		uint32_t ChunkSize;
		uint32_t ChunkCount;
		bool BatchLogData;
		bool UniqueRowsOnly;
		bool IncludeImplicitPC;
		CompressionType CompressionType;
		OutputFormat Format;
	};

	bool GetUniqueRowsOnly() const
	{
		return _sharedSettings.UniqueRowsOnly;
	}

	bool GetIncludeImplictPC() const
	{
		return _sharedSettings.IncludeImplicitPC;
	}

	OutputFormat GetOutputFormat() const
	{
		return _sharedSettings.Format;
	}

protected:
	Settings _sharedSettings;
};