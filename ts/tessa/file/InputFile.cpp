#include "Precompiled.h"
#include "ts/tessa/file/InputFile.h"

#include <cstdio>

TS_PACKAGE1(file)

typedef FILE FileHandle;

InputFile::InputFile()
{
}

InputFile::InputFile(const String &filepath, InputFileMode mode)
{
	open(filepath, mode);
}

InputFile::~InputFile()
{
	close();
}

InputFile::InputFile(InputFile &&other)
{
	*this = std::move(other);
}

InputFile &InputFile::operator=(InputFile &&other)
{
	if (this != &other)
	{
		std::swap(filePtr, other.filePtr);
		std::swap(eof, other.eof);
		std::swap(bad, other.bad);
		std::swap(filesize, other.filesize);
	}
	return *this;
}

bool InputFile::open(const String &filepath, InputFileMode modeParam)
{
	TS_ASSERT(filePtr == nullptr && "InputFile is already opened.");
	if (filePtr != nullptr)
		return false;

	String mode;
	switch (modeParam)
	{
		case InputFileMode_Read:       mode = "r";  break;
		case InputFileMode_ReadBinary: mode = "rb"; break;
		default: TS_ASSERT(!"Unhandled mode"); return false;
	}

#if TS_PLATFORM == TS_WINDOWS
	FileHandle *file = _wfopen(filepath.toWideString().c_str(), mode.toWideString().c_str());
#else
	FileHandle *file = fopen(filepath.toUtf8().c_str(), mode.toUtf8().c_str());
#endif
	if (file == nullptr)
	{
		TS_LOG_ERROR("Open failed. File: %s - Error: %s\n", filepath, strerror(errno));
		return false;
	}

	filePtr = file;
	return true;
}

void InputFile::close()
{
	if (filePtr != nullptr)
	{
		FileHandle *file = static_cast<FileHandle*>(filePtr);
		fclose(file);
	}
	filePtr = nullptr;
	eof = false;
	bad = false;
	filesize = -1;
}

PosType InputFile::read(char *outBuffer, BigSizeType size)
{
	TS_ASSERT(outBuffer != nullptr);
	TS_ASSERT(filePtr != nullptr && "InputFile is not opened.");

	if (filePtr == nullptr || bad == true)
		return -1;

	if (eof == true)
		return 0;

	FileHandle *file = static_cast<FileHandle*>(filePtr);

	PosType numBytesRead = fread(outBuffer, sizeof(outBuffer[0]), size, file);

	if (ferror(file))
	{
		bad = true;
		return -1;
	}
	
	eof = (feof(file) != 0);

	return numBytesRead;
}

PosType InputFile::read(unsigned char *outBuffer, BigSizeType size)
{
	return read(reinterpret_cast<char*>(outBuffer), size);
}

PosType InputFile::readLine(char *outBuffer, BigSizeType size, const char linebreak)
{
	TS_ASSERT(outBuffer != nullptr);
	TS_ASSERT(filePtr != nullptr && "InputFile is not opened.");
	TS_ASSERT(size > 0 && "Size must be greater than 0.");

	if (filePtr == nullptr || bad == true || size == 0)
		return -1;

	if (eof == true)
		return 0;

	char c;
	char *ptr = outBuffer;

	FileHandle *file = static_cast<FileHandle*>(filePtr);
	while (size-- > 0)
	{
		if (fread(&c, sizeof(char), 1, file) == 0)
			break;

		*ptr++ = c;

		if (feof(file) != 0 || c == linebreak)
			break;
	}

	*ptr = '\0';
	return (ptr - outBuffer);
}

PosType InputFile::readLine(unsigned char *outBuffer, BigSizeType size, const char linebreak)
{
	return readLine(reinterpret_cast<char*>(outBuffer), size, linebreak);
}

PosType InputFile::seek(PosType pos)
{
	return seek(pos, SeekFromBeginning);
}

PosType InputFile::seek(PosType pos, SeekOrigin seekOrigin)
{
	TS_ASSERT(filePtr != nullptr && "InputFile is not opened.");
	if (filePtr == nullptr || bad == true)
		return -1;

	int32 origin;
	switch (seekOrigin)
	{
		default:
		case SeekFromBeginning: origin = SEEK_SET; break;
		case SeekFromCurrent:   origin = SEEK_CUR; break;
		case SeekFromEnd:       origin = SEEK_END; break;
	}

	FileHandle *file = static_cast<FileHandle*>(filePtr);
	if (fseek(file, (long)pos, origin) == 0)
	{
		eof = (feof(file) != 0);
		return ftell(file);
	}

	return -1;
}

PosType InputFile::tell() const
{
	TS_ASSERT(filePtr != nullptr && "InputFile is not opened.");
	if (filePtr == nullptr || bad == true)
		return -1;

	FileHandle *file = static_cast<FileHandle*>(filePtr);
	return ftell(file);
}

PosType InputFile::getSize()
{
	TS_ASSERT(filePtr != nullptr && "InputFile is not opened.");
	if (filePtr == nullptr || bad == true)
		return -1;

	// Return cached file size
	if (filesize != -1)
		return filesize;
	
	BigSizeType originalPosition = tell();

	if (seek(0, SeekFromEnd) > 0)
	{
		BigSizeType size = tell();
		seek(originalPosition, SeekFromBeginning);
		return size;
	}

// 	bad = true;
	return -1;
}

bool InputFile::isOpen() const
{
	return filePtr != nullptr && bad == false;
}

bool InputFile::isEOF() const
{
	return eof;
}

bool InputFile::isBad() const
{
	TS_ASSERT(filePtr != nullptr && "InputFile is not opened.");
	return filePtr == nullptr || bad == true;
}

void InputFile::clearFlags()
{
	TS_ASSERT(filePtr != nullptr && "InputFile is not opened.");
	if (filePtr == nullptr)
		return;

	FileHandle *file = static_cast<FileHandle*>(filePtr);
	clearerr(file);
	eof = false;
	bad = false;
}

InputFile::operator bool() const
{
	return isOpen();
}

bool InputFile::operator!() const
{
	return !isOpen();
}

TS_END_PACKAGE1()
