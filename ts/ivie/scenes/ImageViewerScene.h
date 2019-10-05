#pragma once

#include "ts/engine/system/AbstractSceneBase.h"

#include "ts/math/Damper.h"
#include "ts/ivie/image/Image.h"

#include "ts/engine/window/WindowManager.h"

TS_DECLARE2(app, viewer, ViewerManager);
TS_DECLARE_STRUCT2(engine, system, WindowView);

TS_PACKAGE2(app, scenes)

class ImageViewerScene : public engine::system::AbstractSceneBase
{
	TS_DECLARE_SCENE(ImageViewerScene);

public:
	ImageViewerScene(engine::system::BaseApplication *application);
	virtual ~ImageViewerScene();

	virtual bool start();
	virtual void stop();

	virtual void loadResources(resource::ResourceManager &rm) override;

	virtual bool handleEvent(const sf::Event event) override;

	virtual void update(const TimeSpan deltaTime) override;
	virtual void updateFrequent(const TimeSpan deltaTime) override;

	virtual void renderApplication(sf::RenderTarget &renderTarget, const engine::window::WindowView &view) override;
	virtual void renderInterface(sf::RenderTarget &renderTarget, const engine::window::WindowView &view) override;

protected:
	void imageChanged(SharedPointer<image::Image> image);
	lang::SignalBind imageChangedBind;

	void filelistChanged(SizeType numFiles);
	lang::SignalBind filelistChangedBind;

	void screenResized(const math::VC2U &size);
	lang::SignalBind screenResizedBind;

	void filesDropped(const std::vector<engine::window::DroppedFile> &files);
	lang::SignalBind filesDroppedBind;

	resource::FontResource *font = nullptr;

	float framePadding = 20.f;
	
	Clock elapsedTimer;

	Clock clickTimer;
	Clock changeTimer;

	bool updateImageInfo();

	void drawLoaderGadget(sf::RenderTarget &renderTarget,
		const math::VC2 &centerPosition, float width = 12.f);

	resource::ShaderResource *backgroundShader = nullptr;
	resource::ShaderResource *gaussianShader = nullptr;

	struct CurrentState
	{
		SharedPointer<image::Image> image;

		bool hasData = false;
		bool hasError = false;
		image::ImageData data;

		TimeSpan frameTime;
	};
	CurrentState current;

	math::FloatDamper defaultScale;
	math::FloatDamper imageScale;
	math::VC2Damper positionOffset;

	Clock frameTimer;

	void enforceOversizeLimits(float scale, bool enforceTarget = true);
	math::VC2 positionOversizeLimit;

	math::VC2 calculateMouseDiff(const engine::window::WindowView &view, 
		const math::VC2 &mousePos, float currentScale, float targetScale);

	enum DisplayMode
	{
		Normal,
		Manga,
	};
	DisplayMode displayMode = Normal;

	float dragged = 0.f;

	//////////////////////

	enum ViewerInfoMode
	{
		ViewerInfo_DisplayAll,
		ViewerInfo_IndexOnly,
		ViewerInfo_HideAll,
	};
	ViewerInfoMode viewerInfoMode = ViewerInfo_DisplayAll;
	struct ViewerInfoAlpha
	{
		float index = 1.f;
		float other = 1.f;
	};
	ViewerInfoAlpha viewerInfoAlpha;

	bool displaySmooth = true;

	math::VC2I lastMousePosition;

	bool showManagerStatus = false;
	bool showSchedulerStatus = false;

	engine::window::WindowManager *windowManager = nullptr;
	viewer::ViewerManager *viewerManager = nullptr;
};

TS_END_PACKAGE2()
