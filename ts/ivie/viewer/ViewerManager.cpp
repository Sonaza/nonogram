#include "Precompiled.h"
#include "ViewerManager.h"

#include "ts/container/ContainerUtil.h"
#include "ts/file/FileUtils.h"
#include "ts/math/Hash.h"
#include "ts/profiling/ZoneProfiler.h"
#include "ts/resource/ResourceManager.h"
#include "ts/string/StringUtils.h"
#include "ts/thread/AbstractThreadEntry.h"
#include "ts/thread/Thread.h"

#include "ts/ivie/util/NaturalSort.h"
#include "ts/ivie/viewer/SupportedFormats.h"

#include <functional>

TS_DEFINE_MANAGER_TYPE(app::viewer::ViewerManager);

TS_PACKAGE2(app, viewer)

class ViewerManager::BackgroundImageUnloader : public thread::AbstractThreadEntry
{
	ViewerManager *viewerManager = nullptr;

	std::atomic_bool running = true;
	Thread *thread = nullptr;

	ConditionVariable condition;

	std::map<uint32_t, Time> unloadQueue;

public:
	Mutex mutex;

	BackgroundImageUnloader(ViewerManager *viewerManager)
		: viewerManager(viewerManager)
	{
		running = true;
		condition.notifyAll();

		thread = Thread::createThread(this, "ViewerManager::BackgroundImageUnloader");
	}

	~BackgroundImageUnloader()
	{
		running = false;
		if (thread != nullptr)
			Thread::joinThread(thread);
	}

	void addToQueue(uint32_t imageHash, TimeSpan delay)
	{
		TS_ASSERT(viewerManager->imageStorage.find(imageHash) != viewerManager->imageStorage.end() &&
			"Image hash not found in storage, don't try to unload images that aren't even loaded.");

		unloadQueue[imageHash] = Time::now() + delay;
	}

	void removeFromQueue(uint32_t imageHash)
	{
// 		TS_ASSERT(unloadQueue.find(imageHash) != unloadQueue.end() &&
// 			"Image hash not found in unload queue, don't try to cancel unloads.");

		unloadQueue.erase(imageHash);
	}

	void entry()
	{
		while (running)
		{
			MutexGuard lock(mutex);
			condition.waitFor(lock, 50_ms, [this]()
			{
				return !running;// || !unloadQueue.empty();
			});
			if (!running)
				return;

// 			TS_ZONE();

			std::vector<SharedPointer<image::Image>> unloadables;

			for (auto it = unloadQueue.begin(); it != unloadQueue.end();)
			{
				if (Time::now() >= it->second)
				{
					unloadables.push_back(viewerManager->imageStorage[it->first]);
					it = unloadQueue.erase(it);
				}
				else
				{
					++it;
				}
			}

			lock.unlock();

			for (SharedPointer<image::Image> &image : unloadables)
			{
				if (image != nullptr && !image->isUnloaded())
				{
					TS_WPRINTF("---- Unloading image %s\n", image->getFilepath());
					image->unload();
				}
			}

			Thread::sleep(10_ms);
		}
	}

};

std::atomic_bool ViewerManager::quitting = false;

ViewerManager::ViewerManager()
{
	gigaton.registerClass(this);
}

ViewerManager::~ViewerManager()
{
	gigaton.unregisterClass(this);
}

bool ViewerManager::initialize()
{
	TS_ZONE();

	threadScheduler = &getGigaton<thread::ThreadScheduler>();
	
	prepareShaders();

	allowedExtensions = viewer::SupportedFormats::getSupportedFormatExtensions();

	backgroundUnloader.reset(new BackgroundImageUnloader(this));

	return true;
}

void ViewerManager::deinitialize()
{
	TS_ZONE();

	quitting = true;

	if (scannerTaskId != thread::InvalidTaskId)
		threadScheduler->cancelTask(scannerTaskId, true);

	backgroundUnloader.reset();

	for (ImageStorageList::iterator it = imageStorage.begin(); it != imageStorage.end(); ++it)
	{
		SharedPointer<image::Image> &image = it->second;
		if (image && !image->isUnloaded())
			image->unload();
	}
	imageStorage.clear();

	alphaCheckerPatternTexture.reset();
}

void ViewerManager::update(const TimeSpan deltaTime)
{
	TS_ZONE();

	fileWatcher.update();

	if (pendingImageUpdate)
	{
		{
			MutexGuard lock(mutex);

			SizeType previousImageIndex = current.imageIndex;
			uint32_t previousDirectoryHash = current.directoryHash;

			if (pending.imageIndex != INVALID_IMAGE_INDEX)
			{
				current = std::move(pending);
			}
			
			updateCurrentImage(previousDirectoryHash, previousImageIndex);

			pendingImageUpdate = false;
		}

		imageChangedSignal(currentImage);
	}
}

void ViewerManager::setPendingImage(SizeType imageIndex)
{
	if (imageIndex == INVALID_IMAGE_INDEX)
	{
		pending = DisplayState();
		return;
	}

	if (imageIndex < currentFileList.size())
	{
		pending.imageIndex = imageIndex;
		pending.directoryHash = currentDirectoryPathHash;
		pending.viewerFile = currentFileList[imageIndex];
	}
	else
	{
		pending = DisplayState();
		current = pending;
	}

	pendingImageUpdate = true;
}

void ViewerManager::watchNotify(const std::vector<file::FileNotifyEvent> &notifyEvents)
{
	MutexGuard lock(mutex, thread::DeferLock);
	
	bool sortNeeded = false;
	bool ensureImageIndexNeeded = false;

	for (const file::FileNotifyEvent &notifyEvent : notifyEvents)
	{
		if (!isExtensionAllowed(notifyEvent.name))
			continue;

		if (!lock.isLocked())
			lock.lock();

		switch (notifyEvent.flag)
		{
			case file::FileNotify_FileAdded:
			{
				TS_PRINTF("FileNotify_FileAdded: %s\n", notifyEvent.name);

				String fullpath = file::joinPaths(currentDirectoryPath, notifyEvent.name);
				ViewerImageFile file = getViewerImageFileDataForFile(fullpath, notifyEvent.name);
				currentFileList.push_back(file);

				filelistChangedSignal((SizeType)currentFileList.size());
			}
			break;

			case file::FileNotify_FileRemoved:
			{
				TS_PRINTF("FileNotify_FileRemoved: %s\n", notifyEvent.name);
				TS_PRINTF("  current %s\n", current.viewerFile.filepath);

				auto it = std::find_if(
					currentFileList.begin(), currentFileList.end(),
					[&notifyEvent](const ViewerImageFile &x) { return x.filepath == notifyEvent.name; }
				);
				if (it != currentFileList.end())
					currentFileList.erase(it);

				if (!currentFileList.empty())
				{
					if (current.viewerFile.filepath == notifyEvent.name)
						jumpToImage(current.imageIndex);
					else
						ensureImageIndexNeeded = true;
				}

				filelistChangedSignal((SizeType)currentFileList.size());
			}
			break;

			case file::FileNotify_FileRenamed:
			{
				TS_WPRINTF("FileNotify_FileRenamed: %s -> %s\n", notifyEvent.name, notifyEvent.lastName);

				auto it = std::find_if(
					currentFileList.begin(), currentFileList.end(),
					[&notifyEvent](const ViewerImageFile &x) { return x.filepath == notifyEvent.lastName; }
				);
				if (it != currentFileList.end())
				{
					it->filepath = notifyEvent.name;
					sortNeeded = true;
				}

				if (current.viewerFile.filepath == notifyEvent.lastName)
				{
					current.viewerFile.filepath = notifyEvent.name;
					ensureImageIndexNeeded = true;
				}
			}
			break;

			default: /* boop */ break;
		}
	}

	if (sortNeeded)
		applySorting(currentFileList);

	if (ensureImageIndexNeeded)
		ensureImageIndex();
}

void ViewerManager::resetFileWatcher(bool recursive)
{
	if (fileWatcher.isWatching())
	{
		watchNotifyBind.disconnect();
		fileWatcher.reset();
	}

	fileWatcher.watch(currentDirectoryPath, recursive, file::FileWatch_FileChanges | file::FileWatch_DirectoryChanges);
	watchNotifyBind.connect(fileWatcher.notifySignal, &ThisClass::watchNotify, this);
}

void ViewerManager::setViewerPath(const String &filepath)
{
	TS_ZONE();

	MutexGuard lock(mutex);

	if (!file::exists(filepath))
	{
		TS_WLOG_ERROR("Given filepath does not exist. Path: %s.", filepath);
		return;
	}

	String directoryPath = file::getDirname(filepath);
	if (directoryPath.isEmpty())
		directoryPath = file::getWorkingDirectory();

	if (getIsRecursiveScan() && file::pathIsSubpath(currentDirectoryPath, directoryPath))
	{
		if (file::isFile(filepath))
		{
			jumpToImageByFilename(filepath);
		}
		else if (file::isDirectory(filepath))
		{
			jumpToImageByDirectory(filepath);
		}
		
		return;
	}

	if (scannerTaskId != thread::InvalidTaskId)
	{
		threadScheduler->cancelTask(scannerTaskId, true);
		scannerTaskId = thread::InvalidTaskId;
	}

	firstScanComplete = false;
	currentDirectoryPath = directoryPath;
	TS_ASSERT(!currentDirectoryPath.isEmpty());
	currentDirectoryPathHash = math::simpleHash32(currentDirectoryPath);

	resetFileWatcher(getIsRecursiveScan());

	IndexingAction action = IndexingAction_KeepCurrentFile;

	if (file::isFile(filepath) && isExtensionAllowed(filepath))
	{
		const String relativePath = file::stripRootPath(filepath, currentDirectoryPath);

		currentFileList.clear();

		ViewerImageFile file = getViewerImageFileDataForFile(filepath, relativePath);
		currentFileList.push_back(file);

		setPendingImage(0);
	}
	else
	{
		action = IndexingAction_Reset;
	}

	scannerTaskId = threadScheduler->scheduleOnce(
		thread::Priority_Critical,
		TimeSpan::zero,
		&ThisClass::updateFilelist, this, directoryPath, false, action
	).getTaskId();
}

const String &ViewerManager::getViewerPath() const
{
	return currentDirectoryPath;
}

bool ViewerManager::getIsRecursiveScan() const
{
	return scanStyle == file::FileListStyle_Files_Recursive;
}

void ViewerManager::setRecursiveScan(bool recursiveEnabled, bool immediateRescan)
{
	scanStyle = !recursiveEnabled ? file::FileListStyle_Files : file::FileListStyle_Files_Recursive;

	if (!currentDirectoryPath.isEmpty() && immediateRescan)
	{
		if (scannerTaskId != thread::InvalidTaskId)
		{
			threadScheduler->cancelTask(scannerTaskId, true);
			scannerTaskId = thread::InvalidTaskId;
		}

		resetFileWatcher(getIsRecursiveScan());

		firstScanComplete = false;

		scannerTaskId = threadScheduler->scheduleOnce(
			thread::Priority_Critical,
			TimeSpan::zero,
			&ThisClass::updateFilelist, this, currentDirectoryPath, true, IndexingAction_KeepCurrentFile
		).getTaskId();
	}
}

//////////////////////////////////////////////////////

void ViewerManager::jumpToImage(SizeType index)
{
	TS_ZONE();

	if (currentFileList.empty())
		return;

	const SizeType numImagesTotal = (SizeType)currentFileList.size();
	if (index >= numImagesTotal)
		index = numImagesTotal - 1;

// 	if (index == current.imageIndex)
// 		return;

	setPendingImage(index);
}

void ViewerManager::jumpToImageByFilename(const String &filename)
{
	const String relativePath = file::stripRootPath(filename, currentDirectoryPath);

	PosType index = findFileIndexByName(relativePath, currentFileList);
	if (index >= 0)
		jumpToImage((SizeType)index);
	else
		jumpToImage(0);
}

void ViewerManager::jumpToImageByDirectory(const String &filename)
{
	const String relativePath = file::stripRootPath(filename, currentDirectoryPath);

	PosType index = -1;

	auto it = std::find_if(
		currentFileList.begin(), currentFileList.end(),
		[&](const ViewerImageFile &x) { return x.filepath.find(relativePath) == 0; }
	);
	if (it != currentFileList.end())
	{
		index = std::distance(currentFileList.begin(), it);
	}

	if (index >= 0)
		jumpToImage((SizeType)index);
	else
		jumpToImage(0);
}

void ViewerManager::changeToNextImage()
{
	changeImage(1);
}

void ViewerManager::changeToPreviousImage()
{
	changeImage(-1);
}

void ViewerManager::changeImage(int32_t amount)
{
	if (amount == 0)
		return;

	const SizeType numImagesTotal = (SizeType)currentFileList.size();
	if (numImagesTotal == 0)
		return;

	SizeType index = (current.imageIndex + numImagesTotal + amount) % numImagesTotal;
	jumpToImage(index);
}

bool ViewerManager::deleteCurrentImage()
{
	
	if (currentImage == nullptr)
		return false;
	
	const String filepath = currentImage->getFilepath();

	TS_PRINTF("CURRENT FILE (IMAGE): %s\n", filepath);
	TS_PRINTF("CURRENT FILE (META): %s\n", current.viewerFile.filepath);
	currentImage->unload();
	
	if (file::removeFile(filepath))
	{
		return true;
	}
	
	return false;
}

bool ViewerManager::rotateCurrentImage(image::Image::RotateDirection direction)
{
	if (currentImage == nullptr)
		return false;

	return currentImage->rotate(direction);
}

SizeType ViewerManager::getCurrentImageIndex() const
{
	MutexGuard lock(mutex);
	return current.imageIndex;
}

SizeType ViewerManager::getNumImages() const
{
	MutexGuard lock(mutex);
	return (SizeType)currentFileList.size();
}

const std::vector<ViewerImageFile> ViewerManager::getImagesInCurrentDirectory() const
{
	MutexGuard lock(mutex);

	const String dirname = file::getDirname(file::joinPaths(currentDirectoryPath, current.viewerFile.filepath));
	const uint32_t currentDirectoryHash = math::simpleHash32(dirname);

	std::vector<ViewerImageFile> entries;
	for (const ViewerImageFile &entry : currentFileList)
	{
		if (entry.directoryHash == currentDirectoryHash)
			entries.push_back(entry);
	}
	return entries;
}

bool ViewerManager::getImageIndexForCurrentDirectory(SizeType &currentIndexOut, SizeType &numImagesOut) const
{
	if (current.viewerFile.filepath.isEmpty())
		return false;

	const std::vector<ViewerImageFile> images = getImagesInCurrentDirectory();

	numImagesOut = (SizeType)images.size();
	for (SizeType i = 0; i < images.size(); ++i)
	{
		if (images[i].filepath == current.viewerFile.filepath)
		{
			currentIndexOut = i;
			return true;
		}
	}

	return false;
}

bool ViewerManager::isScanningFiles() const
{
	return scanningFiles.load();
}

bool ViewerManager::isFirstScanComplete() const
{
	return firstScanComplete;
}

const String ViewerManager::getCurrentFilepath(bool absolute) const
{
	MutexGuard lock(mutex);
	return absolute ? file::joinPaths(currentDirectoryPath, current.viewerFile.filepath) : current.viewerFile.filepath;
}

//////////////////////////////////////////////////////

void ViewerManager::setSorting(SortingStyle style, bool reversed)
{
	MutexGuard lock(mutex);
	if (sortingStyle == style && sortingReversed == reversed)
		return;

	sortingStyle = style;
	sortingReversed = reversed;

	if (!currentFileList.empty())
	{
		applySorting(currentFileList);
		ensureImageIndex();

		updateCurrentImage(current.directoryHash, current.imageIndex);

		filelistChangedSignal((SizeType)currentFileList.size());
	}
}

SortingStyle ViewerManager::getSortingStyle() const
{
	return sortingStyle;
}

bool ViewerManager::getSortingReversed() const
{
	return sortingReversed;
}

//////////////////////////////////////////////////////

const std::vector<ImageEntry> ViewerManager::getListSliceForBuffering(SizeType numForward, SizeType numBackward)
{
	TS_ZONE();

	std::vector<ImageEntry> result;

	const SizeType fileListSize = (SizeType)currentFileList.size();
	if (fileListSize == 0)
		return result;

	// Always returns the current image
	SizeType numEntries = math::min(fileListSize, 1 + numForward + numBackward);
	result.reserve(numEntries);

	for (SizeType base = 0; base < numForward + 1; ++base)
	{
		SizeType index = (current.imageIndex + base) % fileListSize;
		result.push_back(ImageEntry{ currentFileList[index].filepath, index, ImageEntry::Buffering_Forwards });

		numEntries--;
		if (numEntries == 0)
			break;
	}

	if (numEntries > 0)
	{
		for (SizeType base = 0; base < numBackward; ++base)
		{
			SizeType index = (current.imageIndex + (fileListSize - 1 - (PosType)base)) % fileListSize;
			result.push_back(ImageEntry{ currentFileList[index].filepath, index, ImageEntry::Buffering_Backwards });
		
			numEntries--;
			if (numEntries == 0)
				break;
		}
	}

	return result;
}

bool ViewerManager::isExtensionAllowed(const String &filename)
{
	const String ext = string::toLowercaseCopy(file::getExtension(filename));
	return std::find(allowedExtensions.begin(), allowedExtensions.end(), ext) != allowedExtensions.end();
}

template<class T, class V>
struct ScopedStateSetter
{
	T *ptr;
	V exitvalue;
	ScopedStateSetter(T *ptr, V entryvalue, V exitvalue)
		: ptr(ptr)
		, exitvalue(exitvalue)
	{
		*ptr = entryvalue;
	}
	~ScopedStateSetter()
	{
		*ptr = exitvalue;
	}
};

bool ViewerManager::updateFilelist(const String directoryPath,
	bool allowFullRecursive, IndexingAction indexingAction)
{
	TS_ZONE();

	const thread::SchedulerTaskId taskId = thread::ThreadScheduler::getCurrentThreadTaskId();

	if (quitting)
		return false;

	if (!file::exists(directoryPath) || !file::isDirectory(directoryPath))
	{
		TS_WLOG_ERROR("Directory path does not exist. Path: %s", directoryPath);

		{
			TS_ZONE_NAMED("Copying filelist");
			MutexGuard lock(mutex);
			currentFileList.clear();

			currentDirectoryPath.clear();
			currentDirectoryPathHash = 0;
		
			setPendingImage(INVALID_IMAGE_INDEX);
		}

		filelistChangedSignal(0U);
		
		return false;
	}

	ScopedStateSetter<decltype(scanningFiles), bool> scanningFilesSetter(&scanningFiles, true, false);

	std::vector<ViewerImageFile> templist;

	file::FileListStyle listScanStyle = allowFullRecursive ? scanStyle : file::FileListStyle_Files;
	uint32_t flags = file::FileListFlags_LargeFetch
		         | file::FileListFlags_ExcludeRootPath
		         | file::FileListFlags_GetTypeStrings;

	while (!quitting)
	{
		file::FileList lister(directoryPath, listScanStyle, flags);

		file::FileListEntry entry;
		while (lister.next(entry))
		{
			if (quitting)
				return false;

			if (threadScheduler->isTaskCancelled(taskId))
			{
				TS_PRINTF("Task cancelled, skedaddlar!\n");
				return false;
			}

			if (isExtensionAllowed(entry.getFilename()))
			{
				ViewerImageFile file;
				file.filepath = entry.getFilename();

				String dirpath = file::getDirname(file::joinPaths(directoryPath, entry.getFilename()));
				file.directoryHash = math::simpleHash32(dirpath);

				file.lastModifiedTime = entry.getLastModified();
				file.type = entry.getTypestring();
				templist.push_back(file);
			}
		}

		if (templist.empty() && scanStyle == file::FileListStyle_Files_Recursive && listScanStyle != scanStyle)
		{
			listScanStyle = scanStyle;
			continue;
		}
		break;
	}

	if (quitting || threadScheduler->isTaskCancelled(taskId))
		return false;

	applySorting(templist);

	if (quitting)
		return false;

	{
		TS_ZONE_NAMED("Copying filelist");
		MutexGuard lock(mutex);
		currentFileList = std::move(templist);

		switch (indexingAction)
		{
			case IndexingAction_DoNothing:
				// See me doing nothing here
			break;

			case IndexingAction_KeepCurrentFile:
				TS_PRINTF("IndexingAction_KeepCurrentFile\n");
				ensureImageIndex();
			break;

			case IndexingAction_Reset:
				TS_PRINTF("IndexingAction_Reset\n");
				setPendingImage(0);
			break;
		}
	}
	
	filelistChangedSignal((SizeType)currentFileList.size());

	scanningFiles = false;

	if (quitting)
		return false;

	if (allowFullRecursive == false && scanStyle == file::FileListStyle_Files_Recursive)
	{
		scannerTaskId = threadScheduler->scheduleOnce(
			thread::Priority_Normal,
			TimeSpan::zero,
			&ThisClass::updateFilelist, this, directoryPath, true, IndexingAction_KeepCurrentFile
		).getTaskId();
	}
	else if (allowFullRecursive == true || scanStyle != file::FileListStyle_Files_Recursive)
	{
		scannerTaskId = thread::InvalidTaskId;
		firstScanComplete = true;
	}

	return true;
}

void ViewerManager::applySorting(std::vector<ViewerImageFile> &filelist)
{
	TS_ZONE();

	typedef viewer::ViewerImageFile T;
	typedef bool SortingPredicate(const T &, const T &);

	struct Predicates
	{
		static bool byNameAscending(const T &lhs, const T &rhs)
		{
			return util::naturalSortFile(lhs, rhs);
		}
		static bool byNameDescending(const T &lhs, const T &rhs)
		{
			return !util::naturalSortFile(lhs, rhs);
		}

		static bool byTypeAscending(const T &lhs, const T &rhs)
		{
			return util::naturalSortFileByType(lhs, rhs);
		}
		static bool byTypeDescending(const T &lhs, const T &rhs)
		{
			return !util::naturalSortFileByType(lhs, rhs);
		}

		static bool byLastModifiedAscending(const T &lhs, const T &rhs)
		{
			return util::naturalSortFileByLastModified(lhs, rhs);
		}
		static bool byLastModifiedDescending(const T &lhs, const T &rhs)
		{
			return !util::naturalSortFileByLastModified(lhs, rhs);
		}
	};

	SortingPredicate *predicate = nullptr;
	switch (sortingStyle)
	{
		case SortingStyle_ByName:
			if (!sortingReversed)
				predicate = &Predicates::byNameAscending;
			else
				predicate = &Predicates::byNameDescending;
		break;

		case SortingStyle_ByType:
			if (!sortingReversed)
				predicate = &Predicates::byTypeAscending;
			else
				predicate = &Predicates::byTypeDescending;
		break;

		case SortingStyle_ByLastModified:
			if (!sortingReversed)
				predicate = &Predicates::byLastModifiedAscending;
			else
				predicate = &Predicates::byLastModifiedDescending;
		break;
		
		default:
			TS_PRINTF("Sorting algorithm not defined for this option.\n");
		break;
	}

	if (predicate != nullptr)
		std::sort(filelist.begin(), filelist.end(), predicate);
}

void ViewerManager::ensureImageIndex()
{
	if (currentFileList.empty())
	{
		setPendingImage(INVALID_IMAGE_INDEX);
		current = DisplayState();
		return;
	}

	DisplayState *state = &current;
	if (pendingImageUpdate && pending.imageIndex != INVALID_IMAGE_INDEX)
		state = &pending;

	PosType updatedIndex = findFileIndexByName(state->viewerFile.filepath, currentFileList);
	TS_WPRINTF("File: %s   Updated index: %lld\n", state->viewerFile.filepath, updatedIndex);
	if (updatedIndex != state->imageIndex)
	{
		if (updatedIndex >= 0)
		{
			state->imageIndex = (SizeType)updatedIndex;
		}
		else
		{
			SizeType index = (SizeType)(state->imageIndex > 0 ? state->imageIndex - 1 : 0);
			index = math::min(index, (SizeType)currentFileList.size() - 1);
			jumpToImage(index);
		}
	}
}

PosType ViewerManager::findFileIndexByName(const String &filepath, const std::vector<ViewerImageFile> &filelist)
{
	PosType imageIndex = -1;

	std::vector<ViewerImageFile>::const_iterator it = std::find_if(
		filelist.begin(), filelist.end(),
		[&filepath](const ViewerImageFile &x) { return x.filepath == filepath; });

	if (it != filelist.end())
		imageIndex = std::distance(filelist.begin(), it);

	return imageIndex;
}


String ViewerManager::getStats()
{
	std::vector<String> stats;

	for (ImageStorageList::iterator it = imageStorage.begin(); it != imageStorage.end(); ++it)
	{
		SharedPointer<image::Image> &image = it->second;
		stats.push_back(image->getStats());
	}

	std::sort(stats.begin(), stats.end());

	return string::joinString(stats, "\n");
}

SharedPointer<image::Image> ViewerManager::getCurrentImage() const
{
	TS_ZONE();
	return currentImage;
}

void ViewerManager::prepareShaders()
{
	{
		const SizeType checkerPatternSize = 8;
		const SizeType size = checkerPatternSize * 2;

		// Generate array space for a checker pattern, something like this
		//   XX..
		//   ..XX
		std::vector<uint8_t> patternTexture(size * size * 4,  255);
		for (SizeType y = 0; y < size; ++y)
		{
			for (SizeType x = 0; x < size; ++x)
			{
				if ((y < checkerPatternSize && x < checkerPatternSize) ||
					(y >= checkerPatternSize && x >= checkerPatternSize))
				{
					patternTexture[(y * size + x) * 4 + 0] = 190;
					patternTexture[(y * size + x) * 4 + 1] = 190;
					patternTexture[(y * size + x) * 4 + 2] = 190;
				}
			}
		}

		alphaCheckerPatternTexture = makeShared<sf::Texture>();
		if (alphaCheckerPatternTexture != nullptr)
		{
			alphaCheckerPatternTexture->create(size, size);
			alphaCheckerPatternTexture->update(&patternTexture[0], size, size, 0, 0);
			alphaCheckerPatternTexture->setRepeated(true);
		}
	}

	displayShaderFiles.insert(std::make_pair(DisplayShader_FreeImage, "shader/convert_freeimage.frag"));
	displayShaderFiles.insert(std::make_pair(DisplayShader_Webm,      "shader/convert_webm.frag"));
}

SharedPointer<resource::ShaderResource> ViewerManager::loadDisplayShader(DisplayShaderTypes type)
{
	TS_ASSERT(alphaCheckerPatternTexture);
	TS_ASSERT(displayShaderFiles.find(type) != displayShaderFiles.end() && "Attempting to load an undefined display shader.");

	SharedPointer<resource::ShaderResource> displayShader = makeShared<resource::ShaderResource>(displayShaderFiles[type]);
	TS_ASSERT(displayShader);
	if (displayShader == nullptr)
		return nullptr;

	if (!displayShader->loadResource())
		return nullptr;

	sf::Shader *shader = displayShader->getResource().get();

	if (type == DisplayShader_FreeImage)
	{
		shader->setUniform("u_checkerPatternTexture", *alphaCheckerPatternTexture);
	}

	return displayShader;
}

void ViewerManager::updateCurrentImage(SizeType previousDirectoryHash, SizeType previousImageIndex)
{
	TS_ZONE();

	const PosType numForwardBuffered = 2;
	const PosType numBackwardBuffered = 2;

	std::vector<uint32_t> activeImages;
	std::vector<ImageEntry> imagesToLoad = getListSliceForBuffering(numForwardBuffered, numBackwardBuffered);

	if (imagesToLoad.empty())
		currentImage = nullptr;

	// uint32_t currentImageHash = 0;

	for (const ImageEntry &entry : imagesToLoad)
	{
		uint32_t imageHash = math::hashCombine(currentDirectoryPathHash, entry.filepath);

		activeImages.push_back(imageHash);

		SharedPointer<image::Image> &image = imageStorage[imageHash];
		if (image == nullptr)
		{
			String absolutePath = file::joinPaths(currentDirectoryPath, entry.filepath);
			image = makeShared<image::Image>(absolutePath);
		}

		bool isCurrentImage = (entry.index == current.imageIndex);
		if (isCurrentImage)
			currentImage = image;

		if (image->hasError())
			continue;

		if (image->isUnloaded())
		{
			image->startLoading(!isCurrentImage);
		}
		else if (isCurrentImage && image->isSuspended())
		{
			image->resumeLoading();
		}
		else if (entry.index == (SizeType)previousImageIndex && !isCurrentImage)
		{
			image->restart(true);
		}
		
		if (image->hasError())
		{
			TS_WPRINTF("Couldn't load image: %s\n", image->getErrorText());
			continue;
		}

		image->setActive(isCurrentImage);
	}

	std::vector<uint32_t> newlyActiveImages;
	for (uint32_t imageHash : activeImages)
	{
		if (!ts::util::findIfContains(lastActiveImages, imageHash))
			newlyActiveImages.push_back(imageHash);
	}

	std::vector<uint32_t> newlyInactiveImages;
	for (uint32_t imageHash : lastActiveImages)
	{
		if (!ts::util::findIfContains(activeImages, imageHash))
			newlyInactiveImages.push_back(imageHash);
	}

	lastActiveImages = activeImages;

	{
		MutexGuard lock(backgroundUnloader->mutex);

		for (const uint32_t imageHash : newlyActiveImages)
		{
	 		SharedPointer<image::Image> &image = imageStorage[imageHash];
			
			if (image->getState() == image::Image::Unloaded)
				continue;

			image->restart(true);
			backgroundUnloader->removeFromQueue(imageHash);
		}

		for (const uint32_t imageHash : newlyInactiveImages)
		{
			SharedPointer<image::Image> &image = imageStorage[imageHash];

			if (image->getState() == image::Image::Unloaded)
				continue;

			image->suspendLoader();
			backgroundUnloader->addToQueue(imageHash, 2000_ms);
		}
	}
}

TS_END_PACKAGE2()



