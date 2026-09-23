#pragma once
#include "pch.h"

#include "Utilities/Socket.h"
#include "Utilities/CRC32.h"
#include "Core/Shared/TraceFormat.h"
#include "Core/Shared/MessageManager.h"
#include "Utilities/CompressionHelper.h"

#include <memory>
#include <thread>
#include <mutex>
#include <queue>
#include <atomic>

class TraceLogNetworkSocket
{
private:
	enum class State
	{
		Uninitialized,
		Initialized,
		SocketCreated,
		Listening,
		Connected,
		Logging,
	};

	// A simple explanation on logger's general functionality:
	// - The main thread appends all traces to a ChunkSize-sized buffer from the logging
	//   queue when calling Log().
	// - Once the buffer is full, it is dequeued and pushed to the compression queue.
	// - A second thread dequeues a buffer from the compression queue and zlib-compresses
	//   it into a CompressedChunkSize-sized buffer.
	// - The original buffer is returned to the logging queue, and the compressed buffer
	//   pushed to the networking queue.
	// - A third thread dequeues a buffer from the networking queue, sends it to all
	//   current connections, and pushes it back into the compression queue.
	// Not much testing has been done to verify all of this is necessary for performance,
	// but the original bsnes-plus implementation of this code as used by DiztinGUIsh
	// functioned very similarly, and apparently it was the only way to achieve decent
	// performance. However, I can imagine this could be improved further by switching
	// from TCP to UDP for sending data, which should be fine for the kind of data
	// we're sending here (especially when used for the purpose of disassembling ROMs).
	// Though I won't be doing that until I know the networking thread is actually
	// a bottleneck.

	static constexpr size_t ChunkSize = 1u * 1024u * 1024u;
	static constexpr size_t CompressedChunkSize = (size_t)((float)ChunkSize * 1.2f);
	static constexpr size_t ChunkCount = 10u;

	struct WorkItem
	{
		uint8_t* Buffer;
		size_t Capacity;
		size_t Size;
	};

	uint8_t* _logMemoryStart;
	uint8_t* _compressionMemoryStart;

	WorkItem _currentLogItem;
	uint64_t _lastLogTime;
	std::queue<WorkItem> _loggingQueue;
	std::queue<WorkItem> _compressionInputQueue;
	std::queue<WorkItem> _compressionOutputQueue;
	std::queue<WorkItem> _networkingQueue;

	// This bool is a (probably bad) attempt at preventing a race condition
	// that can happen during shutdown because Log() and StopLogging() are
	// called from different threads. Because Log() is a performance-critical
	// function, I don't want to protect it with a mutex.
	volatile bool _insideLogFunction;
	bool _compressionThreadQuitRequested;
	bool _networkingThreadQuitRequested;
	std::thread _compressionThread;
	std::thread _networkingThread;
	std::mutex _compressionInputMutex;
	std::mutex _compressionOutputMutex;

	std::atomic<State> _state = State::Uninitialized;
	std::unique_ptr<Socket> _socket = nullptr;
	std::vector<std::unique_ptr<Socket>> _openConnections;

	struct ZlibCompressionHeader
	{
		// 0:     uint8_t:  Character 'Z' as a flag for compressed data.
		// 1 - 4: uint32_t: Length of the uncompressed buffer, little-endian.
		// 5 - 8: uint32_t: Length of the compressed buffer, little-endian.
		uint8_t data[9u];		
	};

public:
	~TraceLogNetworkSocket()
	{
		CloseSocket();
	}


	bool OpenSocket(uint16_t port)
	{
		if (_state == State::Uninitialized) {
			_logMemoryStart = new uint8_t[ChunkSize * ChunkCount];

			if (_logMemoryStart == nullptr) {
				return false;
			}

			_compressionMemoryStart = new uint8_t[CompressedChunkSize * ChunkCount];

			if (_compressionMemoryStart == nullptr) {
				delete _logMemoryStart;
				return false;
			}

			_loggingQueue = std::queue<WorkItem>();
			_compressionInputQueue = std::queue<WorkItem>();
			_compressionOutputQueue = std::queue<WorkItem>();
			_networkingQueue = std::queue<WorkItem>();

			for (size_t i = 0; i < ChunkCount; ++i) {
				WorkItem workItem;
				workItem.Buffer = _logMemoryStart + (i * ChunkSize);
				workItem.Capacity = ChunkSize;
				workItem.Size = 0u;
				_loggingQueue.push(workItem);
			}

			for (size_t i = 0; i < ChunkCount; ++i) {
				WorkItem workItem;
				workItem.Buffer = _compressionMemoryStart + (i * CompressedChunkSize);
				workItem.Capacity = CompressedChunkSize;
				workItem.Size = 0u;
				_compressionOutputQueue.push(workItem);
			}

			_state = State::Initialized;	
		}

		if (_state == State::Initialized) {
      	_socket = std::make_unique<Socket>();

			if (_socket->ConnectionError()) {
				_socket.reset(nullptr);
				CloseSocket();
				return false;
			}

			_socket->Bind(port);

			if (_socket->ConnectionError()) {
				_socket.reset(nullptr);
				CloseSocket();
				return false;
			}

			_state = State::SocketCreated;
		}

		if (_state == State::SocketCreated) {
			_socket->Listen(10);

			if (_socket->ConnectionError())
			{
				CloseSocket();
				return false;
			}

			_state = State::Listening;
		}

		return true;
	}

	void CloseSocket()
	{
		EndConnection();

		if (_state == State::Listening) {
			_state = State::SocketCreated;
		}

		if (_state == State::SocketCreated) {
			_socket->Close();
			_socket.reset(nullptr);
			_state = State::Initialized;
		}

		if (_state == State::Initialized) {
			delete _logMemoryStart;
			_logMemoryStart = nullptr;

			delete _compressionMemoryStart;
			_compressionMemoryStart = nullptr;

			_loggingQueue = std::queue<WorkItem>();
			_compressionInputQueue = std::queue<WorkItem>();
			_compressionOutputQueue = std::queue<WorkItem>();
			_networkingQueue = std::queue<WorkItem>();

			_state = State::Uninitialized;
		}
	}


	bool BeginConnection()
	{
		if (_state == State::Listening) {
			if (AcceptConnection()) {
				_state = State::Connected;
				return true;
			}
		}
		
		return false;
	}

	void EndConnection()
	{
		StopLogging();

		if (_state == State::Connected) {
			for (std::unique_ptr<Socket>& socket : _openConnections) {
				socket->Close();
			}

			_openConnections.clear();
			_state = State::Listening;
		}
	}


	bool StartLogging()
	{
		if (_state == State::Connected) {
			_insideLogFunction = false;

			_compressionThreadQuitRequested = false;
			_networkingThreadQuitRequested = false;

			_currentLogItem = _loggingQueue.front();
			_loggingQueue.pop();

			_compressionThread = std::thread([this](){ RunCompression(); });
			_networkingThread = std::thread([this](){ RunNetworking(); });

			_lastLogTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

			_state = State::Logging;
		}
		
		return true;
	}

	void StopLogging()
	{
		if(_state == State::Logging) {
			// In this particular case, because of a possible race condition with the Log() function
			// we set the new state immediately and then spin-lock until the Log() function is done.
			// This way we make sure it can't be called again while our shutdown is active. Adding
			// a mutex here would probably be bad for performance, since Log() gets called thousands
			// of times within a frame.
			_state = State::Connected;

			while(_insideLogFunction) {
				std::this_thread::sleep_for(5ms);
			}

			{
				std::lock_guard<std::mutex> guard(_compressionInputMutex);

				if (_currentLogItem.Size > 0)
				{
					_compressionInputQueue.push(_currentLogItem);
				}
				else
				{
					_loggingQueue.push(_currentLogItem);
				}

				_currentLogItem.Buffer = nullptr;
			}

			_compressionThreadQuitRequested = true;
			_compressionThread.join();
			_networkingThreadQuitRequested = true;
			_networkingThread.join();
		}
	}


	__forceinline bool IsEnabled() { return _state >= State::Logging; }


	void Log(const uint8_t* data, int length)
	{
		_insideLogFunction = true;

		if(_state != State::Logging) {
			_insideLogFunction = false;
			return;
		}

		_lastLogTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

		if (_currentLogItem.Size + length > _currentLogItem.Capacity) {
			if (_currentLogItem.Size == 0u) {
				// If this log entry does not fit into our current buffer,
				// yet the current buffer's size is still at 0, it means the
				// log entry is larger than our full buffer size. There is
				// absolutely no way this entry will ever fit into the buffer,
				// so just discard it. Not much else we can do.
				MessageManager::Log("[Network Socket] Discarding log entry of size " + std::to_string(length) + " because it exceeds the maximum buffer size.");
				_insideLogFunction = false;
				return;
			}

			FlushCurrentWorkItem();
		}

		std::memcpy(_currentLogItem.Buffer + _currentLogItem.Size, data, length);
		_currentLogItem.Size += length;

		_insideLogFunction = false;
	}

	void FlushCurrentWorkItem()
	{
		{
			std::lock_guard<std::mutex> guard(_compressionInputMutex);

			_compressionInputQueue.push(_currentLogItem);
			_currentLogItem.Buffer = nullptr;

			if (!_loggingQueue.empty())
			{
				_currentLogItem = _loggingQueue.front();
				_loggingQueue.pop();
			}
		}

		// In the unlikely case we ever run out of buffers, sleep until one becomes available.
		while (_currentLogItem.Buffer == nullptr) {
			std::this_thread::sleep_for(25ms);

			{
				std::lock_guard<std::mutex> guard(_compressionInputMutex);

				if (!_loggingQueue.empty()) {
					_currentLogItem = _loggingQueue.front();
					_loggingQueue.pop();
				}
			}
		}
	}

	void FlushOldBuffers()
	{
		if (_state != State::Logging) {
			return;
		}

		if (_currentLogItem.Size == 0u) {
			return;
		}

		uint64_t currentTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

		if (currentTime - _lastLogTime >= 5000u) {
			FlushCurrentWorkItem();
		}
	}


	void RunCompression()
	{
		while(!_compressionThreadQuitRequested) {
			FlushCompressionQueue();
			std::this_thread::sleep_for(25ms);
		}

		FlushCompressionQueue();
	}

	void FlushCompressionQueue()
	{
		WorkItem currentItem;
		FetchNextCompressionItem(&currentItem);

		while (currentItem.Buffer != nullptr) {
			WorkItem outputItem;
			outputItem.Buffer = nullptr;

			while (outputItem.Buffer == nullptr) {
				{
					std::lock_guard<std::mutex> guard(_compressionOutputMutex);

					if (!_compressionOutputQueue.empty())
					{
						outputItem = _compressionOutputQueue.front();
						_compressionOutputQueue.pop();
					}
				}

				if (outputItem.Buffer == nullptr)
				{
					std::this_thread::sleep_for(25ms);
				}
			}

			ZlibCompressionHeader header;
			header.data[0] = 'Z';
			header.data[1] = (currentItem.Size >> 0) & 0xFF;
			header.data[2] = (currentItem.Size >> 8) & 0xFF;
			header.data[3] = (currentItem.Size >> 16) & 0xFF;
			header.data[4] = (currentItem.Size >> 24) & 0xFF;

			outputItem.Size = 9;

			mz_ulong compressedSize = (mz_ulong)(outputItem.Capacity - outputItem.Size);
			int z_result = compress(outputItem.Buffer + outputItem.Size, &compressedSize, currentItem.Buffer, (mz_ulong)currentItem.Size);

			{
				std::lock_guard<std::mutex> guard(_compressionInputMutex);

				currentItem.Size = 0u;
				_loggingQueue.push(currentItem);
			}

    		if (z_result != Z_MEM_ERROR && z_result != Z_BUF_ERROR) {
				outputItem.Size += compressedSize;

				header.data[5] = (compressedSize >> 0) & 0xFF;
				header.data[6] = (compressedSize >> 8) & 0xFF;
				header.data[7] = (compressedSize >> 16) & 0xFF;
				header.data[8] = (compressedSize >> 24) & 0xFF;

				memcpy(outputItem.Buffer, &header, sizeof(header));

				{
					std::lock_guard<std::mutex> guard(_compressionOutputMutex);

					_networkingQueue.push(outputItem);
				}
			} else {
				// If there was some compression error, we just discard the result.
				// The most likely cause is the output buffer being too small.
				outputItem.Size = 0u;

				std::lock_guard<std::mutex> guard(_compressionOutputMutex);

				_compressionOutputQueue.push(outputItem);
			}

			FetchNextCompressionItem(&currentItem);
		}
	}

	void FetchNextCompressionItem(WorkItem* targetItem)
	{
		targetItem->Buffer = nullptr;
		
		std::lock_guard<std::mutex> guard(_compressionInputMutex);

		if (!_compressionInputQueue.empty()) {
			*targetItem = _compressionInputQueue.front();
			_compressionInputQueue.pop();
		}
	}


	void RunNetworking()
	{
		while(!_networkingThreadQuitRequested) {
			FlushNetworkingQueue();
			std::this_thread::sleep_for(25ms);
		}

		FlushNetworkingQueue();
	}

	void FlushNetworkingQueue()
	{
		WorkItem currentItem;
		FetchNextNetworkingItem(&currentItem);

		while (currentItem.Buffer != nullptr) {
			AcceptConnection();

			for (auto it = _openConnections.begin(); it != _openConnections.end(); ) {
				std::unique_ptr<Socket>& socket = *it;

				socket->Send((char*)currentItem.Buffer, (int)currentItem.Size, 0u);

				// Erase a connection if it produced a connection error. Probably not much else
				// we can do here. The "AcceptConnection()" above should hopefully re-establish
				// the connection if desired.
				if (socket->ConnectionError()) {
					it = _openConnections.erase(it);
				} else {
					++it;
				}
			}

			{
				currentItem.Size = 0u;

				std::lock_guard<std::mutex> guard(_compressionOutputMutex);
				_compressionOutputQueue.push(currentItem);
			}

			FetchNextNetworkingItem(&currentItem);
		}
	}

	void FetchNextNetworkingItem(WorkItem* targetItem)
	{
		targetItem->Buffer = nullptr;
		
		std::lock_guard<std::mutex> guard(_compressionOutputMutex);

		if (!_networkingQueue.empty()) {
			*targetItem = _networkingQueue.front();
			_networkingQueue.pop();
		}
	}


private:
	bool AcceptConnection()
	{
		unique_ptr<Socket> socket = _socket->Accept();

		if (socket->ConnectionError()) {
			return false;
		}

		_openConnections.push_back(std::move(socket));
		return true;
	}
};