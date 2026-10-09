#pragma once
#include "pch.h"
#include "TraceLogOutput.h"

class TraceLogOutputFile : public TraceLogOutput
{
public:
	struct Settings
	{
		// This is coming directly from C# code. I don't know
		// how long this memory is guaranteed to be valid, which
		// might be a problem if we ever want to access the filename
		// after copying the struct over.
		const char* FileName;
		bool NativeNewLines;
	};

private:
	Settings _selfSettings;

	bool _enabled = false;
	string _outputFilepath;
	string _outputBuffer;
	ofstream _outputFile;

public:
	~TraceLogOutputFile()
	{
		StopLogging();
	}

	bool StartLogging(const Settings& selfSettings, const TraceLogOutput::Settings& sharedSettings)
	{
		_selfSettings = selfSettings;
		_sharedSettings = sharedSettings;

		_outputBuffer.clear();

		std::ios_base::openmode openMode = ios::out;

		if(!_selfSettings.NativeNewLines) {
			openMode |= ios::binary;
		}

		_outputFile.open(_selfSettings.FileName, openMode);
		_enabled = true;

		return true;
	}

	void StopLogging()
	{
		if(_enabled) {
			_enabled = false;
			if(_outputFile) {
				if(!_outputBuffer.empty()) {
					_outputFile << _outputBuffer;
				}
				_outputFile.close();
			}
		}
	}

	__forceinline bool IsEnabled() { return _enabled; }

	void Log(string& log)
	{
		_outputBuffer += log + '\n';
		if(_outputBuffer.size() > 32768) {
			_outputFile << _outputBuffer;
			_outputBuffer.clear();
		}
	}
};