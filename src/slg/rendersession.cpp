/***************************************************************************
 * Copyright 1998-2020 by authors (see AUTHORS.txt)                        *
 *                                                                         *
 *   This file is part of LuxCoreRender.                                   *
 *                                                                         *
 * Licensed under the Apache License, Version 2.0 (the "License");         *
 * you may not use this file except in compliance with the License.        *
 * You may obtain a copy of the License at                                 *
 *                                                                         *
 *     http://www.apache.org/licenses/LICENSE-2.0                          *
 *                                                                         *
 * Unless required by applicable law or agreed to in writing, software     *
 * distributed under the License is distributed on an "AS IS" BASIS,       *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.*
 * See the License for the specific language governing permissions and     *
 * limitations under the License.                                          *
 ***************************************************************************/

#include <mutex>
#include <sstream>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/serialization/shared_ptr.hpp>

#include "slg/rendersession.h"
#include "slg/renderstate.h"
#include "slg/cameras/camera.h"
#include "luxrays/utils/safesave.h"

using namespace std;
using namespace luxrays;
using namespace slg;

void (*slg::LuxRays_DebugHandler)(const char *msg) = NULL;
void (*slg::SLG_DebugHandler)(const char *msg) = NULL;

// Empty debug handler
void slg::NullDebugHandler(const char *msg) {
}

RenderSession::RenderSession(
	RenderConfigRef rcfg,
	RenderStateSPtr startState,
	FilmPtr startFilm
) : renderConfig(rcfg) {
	SDL_LOG("Creating session");

	const double now = WallClockTime();
	lastPeriodicFilmOutputsSave = now;
	lastPeriodicFilmSave = now;
	lastResumeRenderingSave = now;

	//--------------------------------------------------------------------------
	// Create the Film
	//--------------------------------------------------------------------------

	film = renderConfig.AllocFilm();

	//--------------------------------------------------------------------------
	// Create the RenderEngine
	//--------------------------------------------------------------------------

	renderEngine = renderConfig.AllocRenderEngine();
	renderEngine->SetRenderState(startState, startFilm);
}

RenderSession::~RenderSession() {
	if (renderEngine->IsInSceneEdit()) {
		EndSceneEdit();
	}
	if (renderEngine->IsStarted()) {
		Stop();
	}
	renderEngine.reset();
	film.reset();
}

void RenderSession::Start() {
	// Allocate the replacement film first but publish it only after the
	// engine's Start() has initialized it (InitFilm): readers evaluate
	// film-> under filmMutex and must never observe a channel-less,
	// still-initializing Film
	FilmUPtr newFilm;
	if (film->IsInitiliazed()) {
		// I need to allocate a new film because the current one has already been
		// used. For instance, it can happen when stopping and starting the
		// same session.
		newFilm = renderConfig.AllocFilm();
	}
	Film &startedFilm = newFilm ? *newFilm : *film;

	renderEngine->Start(startedFilm, &filmMutex);

	if (newFilm) {
		// filmMutex covers the film pointer swap + destruction of the old
		// Film: FilmImplSession readers evaluate film-> under this lock,
		// so an unlocked swap was a use-after-free window
		std::unique_lock<std::mutex> lock(filmMutex);
		film = std::move(newFilm);
	}

	PublishViewportCamera(true);
}

void RenderSession::Stop() {
	// Force the last update of periodic saves
	CheckPeriodicSave(true);

	renderEngine->Stop();
}

void RenderSession::BeginSceneEdit() {
	renderEngine->BeginSceneEdit();
}

void RenderSession::EndSceneEdit() {
	// Make a copy of the edit actions
	const EditActionList editActions(renderConfig.GetScene().GetEditActions());

	if ((renderEngine->GetType() != RTPATHOCL) &&
			(renderEngine->GetType() != RTPATHCPU)) {
		SLG_LOG("[RenderSession] Edit actions: " << editActions);

		// RTPATHs handle film Reset on their own
		if (editActions.HasAnyAction())
			film->Reset();
	}

	// Publish the camera AFTER the engine-side scene preprocessing:
	// parsed cameras are created with a dummy 100x100 raster and
	// Preprocess() inside EndSceneEdit() is what updates them to the
	// real film resolution, so the warp matrices would be broken here
	renderEngine->EndSceneEdit(editActions);

	PublishViewportCamera(editActions.HasOnly(CAMERA_EDIT));
}

void RenderSession::PublishViewportCamera(const bool cameraOnly) {
	// filmMutex serializes the filmMetadata std::map writes against
	// FilmImplSession readers: the image pipeline's GetMetadata() lookups
	// run under the same lock inside Apply().
	std::unique_lock<std::mutex> lock(filmMutex);

	// Camera reprojection data for the VIEWPORT_TEMPORAL imagepipeline
	// plugin. The plugin warps the previous frame into not-yet-sampled
	// pixels after camera edits; non-camera edits invalidate the history
	// instead (warped colors would be wrong). Film metadata survives
	// Film::Reset(), so the plugin can read it after the render
	// restarts. The plugin keeps its own history camera, so only the
	// current transforms are published here.
	if (!film)
		return;

	const Camera &cam = renderConfig.GetScene().GetCamera();
	const Transform &rtoC = cam.GetRasterToCamera(0);
	const Transform &ctoW = cam.GetCameraToWorld(0);

	auto matToString = [](const Matrix4x4 &m) {
		std::ostringstream ss;
		ss.precision(9);
		for (u_int r = 0; r < 4; ++r)
			for (u_int c = 0; c < 4; ++c)
				ss << m.m[r][c] << " ";
		return ss.str();
	};

	film->SetMetadata("viewport.cam.rtoc", matToString(rtoC.GetMatrix()));
	film->SetMetadata("viewport.cam.ctow", matToString(ctoW.GetMatrix()));
	film->SetMetadata("viewport.cam.wtor",
			matToString((ctoW * rtoC).GetMatrix().Inverse()));
	film->SetMetadata("viewport.cam.wtoc",
			matToString(ctoW.GetMatrix().Inverse()));
	film->SetMetadata("viewport.cam.persp",
			(cam.GetType() == Camera::PERSPECTIVE) ? "1" : "0");
	{
		std::ostringstream ss;
		ss.precision(9);
		ss << cam.clipHither;
		film->SetMetadata("viewport.cam.hither", ss.str());
	}
	film->SetMetadata("viewport.edit.cameraonly", cameraOnly ? "1" : "0");
}

void RenderSession::Pause() {
	renderEngine->Pause();
}

void RenderSession::Resume() {
	renderEngine->Resume();
}

bool RenderSession::HasPeriodicFilmOutputsSave() {
	const double period = renderConfig.GetProperty("periodicsave.film.outputs.period").Get<double>();

	return (period > 0.0);
}

bool RenderSession::HasPeriodicFilmSave() {
	const double period = renderConfig.GetProperty("periodicsave.film.period").Get<double>();

	return (period > 0.0);
}

bool RenderSession::HasResumeRenderingSave() {
	const double period = renderConfig.GetProperty("periodicsave.resumerendering.period").Get<double>();

	return (period > 0.0);
}

bool RenderSession::NeedPeriodicFilmOutputsSave(const bool force) {
	const double period = renderConfig.GetProperty("periodicsave.film.outputs.period").Get<double>();
	if (period > 0.0) {
		if (force)
			return true;

		const double now = WallClockTime();
		if (now - lastPeriodicFilmOutputsSave > period) {
			lastPeriodicFilmOutputsSave = now;
			return true;
		} else
			return false;
	} else
		return false;
}

bool RenderSession::NeedPeriodicFilmSave(const bool force) {
	const double period = renderConfig.GetProperty("periodicsave.film.period").Get<double>();
	if (period > 0.0) {
		if (force)
			return true;

		const double now = WallClockTime();
		if (now - lastPeriodicFilmSave > period) {
			lastPeriodicFilmSave = now;
			return true;
		} else
			return false;
	} else
		return false;
}

bool RenderSession::NeedResumeRenderingSave(const bool force) {
	const double period = renderConfig.GetProperty("periodicsave.resumerendering.period").Get<double>();
	if (period > 0.0) {
		if (force)
			return true;

		const double now = WallClockTime();
		if (now - lastResumeRenderingSave > period) {
			lastResumeRenderingSave = now;
			return true;
		} else
			return false;
	} else
		return false;
}

void RenderSession::SaveFilm(const string &fileName) {
	SLG_LOG("Saving film: " << fileName);

	// Ask the RenderEngine to update the film
	renderEngine->UpdateFilm();

	// renderEngine->UpdateFilm() uses the film lock on its own
	std::unique_lock<std::mutex> lock(filmMutex);

	if (renderConfig.GetProperty("film.safesave").Get<bool>()) {
		SafeSave safeSave(fileName);

		Film::SaveSerialized(safeSave.GetSaveFileName(), *film);

		safeSave.Process();
	} else
		Film::SaveSerialized(fileName, *film);
}

void RenderSession::SaveFilmOutputs() {
	// Ask the RenderEngine to update the film
	renderEngine->UpdateFilm();

	// renderEngine->UpdateFilm() uses the film lock on its own
	std::unique_lock<std::mutex> lock(filmMutex);

	// Save the film
	film->Output();
}

RenderStateSPtr RenderSession::GetRenderState() {
	// Check if we are in the right state
	if (!IsInPause())
		throw runtime_error("A rendering state can be retrieved only while the rendering session is paused");

	return renderEngine->GetRenderState();
}

void RenderSession::Parse(luxrays::PropertiesRPtr props) {
	assert (renderEngine->IsStarted());

	if ((props->IsDefined("film.width") && (props->Get("film.width").Get<u_int>() != film->GetWidth())) ||
			(props->IsDefined("film.height") && (props->Get("film.height").Get<u_int>() != film->GetHeight()))) {
		// I have to use a special procedure if the parsed props include
		// a film resize
		renderEngine->BeginFilmEdit();

		// Update render config properties
		renderConfig.UpdateFilmProperties(*props);

		// Create the new film. It is NOT published to session->film yet:
		// the Film is initialized inside the engine's Start() (InitFilm),
		// so installing the pointer here would let readers observe a film
		// with no channels at all.
		FilmUPtr newFilm = renderConfig.AllocFilm();

		// I have to update the camera
		renderConfig.GetScene().PreprocessCamera(newFilm->GetWidth(), newFilm->GetHeight(), newFilm->GetSubRegion());

		// The engine keeps a FilmRef to the heap object; moving the
		// unique_ptr afterwards does not invalidate it
		renderEngine->EndFilmEdit(*newFilm, &filmMutex);

		{
			// filmMutex covers the film pointer swap + destruction of the
			// old Film: FilmImplSession readers evaluate film-> under this
			// lock, so an unlocked swap was a use-after-free window. It
			// must NOT be held across EndFilmEdit(): that acquires the
			// engine mutex while UpdateFilm() takes the same two locks in
			// the opposite order.
			std::unique_lock<std::mutex> lock(filmMutex);
			film = std::move(newFilm);
		}

		// The raster transform changed with the film size: republish
		// so VIEWPORT_TEMPORAL does not warp with stale dimensions
		PublishViewportCamera(false);
	} else {
		std::unique_lock<std::mutex> lock(filmMutex);
		film->Parse(props);
		// Cryptomatte manifests follow the parsed channel set
		renderConfig.InjectCryptomatteManifests(*film);

		// Update render config properties
		renderConfig.UpdateFilmProperties(*props);
	}
}

void RenderSession::CheckPeriodicSave(const bool force) {
	// Film outputs periodic save
	if (NeedPeriodicFilmOutputsSave(force))
		SaveFilmOutputs();

	// Film periodic save
	if (NeedPeriodicFilmSave(force)) {
		const string fileName = renderConfig.GetProperty("periodicsave.film.filename").Get<string>();

		SaveFilm(fileName);
	}

	// Rendering resume periodic save
	if (NeedResumeRenderingSave(force)) {
		// The .rsm file can be save only during a pause
		Pause();

		const string fileName = renderConfig.GetProperty("periodicsave.resumerendering.filename").Get<string>();
		SaveResumeFile(fileName);
		
		Resume();
	}
}

static size_t SaveRsmFile(RenderSession *renderSession, const std::string &fileName) {
	SerializationOutputFile sof(fileName);

	// Save the render configuration and the scene
	sof.GetArchive() << renderSession->renderConfig;

	// Save the render state
	auto renderState = renderSession->GetRenderState();
	sof.GetArchive() << renderState;
	renderState.reset();

	// Save the film
	sof.GetArchive() << renderSession->film;

	if (!sof.IsGood())
		throw runtime_error("Error while saving serialized render configuration: " + fileName);

	sof.Flush();

	return sof.GetPosition();
}

void RenderSession::SaveResumeFile(const string &fileName) {
	size_t fileSize;

	if (renderConfig.GetProperty("resumerendering.filesafe").Get<bool>()) {
		SafeSave safeSave(fileName);
		
		fileSize = SaveRsmFile(this, safeSave.GetSaveFileName());
	
		safeSave.Process();
	} else
		fileSize = SaveRsmFile(this, fileName);

	SLG_LOG("Render configuration saved: " << (fileSize / 1024) << " Kbytes");
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
