#pragma once

#include "cinder/gl/gl.h"
#include "cinder/Log.h"

#include "openvr.h"

namespace hmd {
	typedef std::shared_ptr<class RenderModel> RenderModelRef;

	class RenderModel {
	public:
		static RenderModelRef create(
			const std::string & name,
			const vr::RenderModel_t & vrModel,
			const vr::RenderModel_TextureMap_t & texture,
			ci::gl::GlslProgRef shader )
		{
			return RenderModelRef( new RenderModel{ name, vrModel, texture, shader } );
		}
		void draw();
		const std::string & GetName() const { return mModelName; }
	private:
		RenderModel( const std::string & name, const vr::RenderModel_t & vrModel, const vr::RenderModel_TextureMap_t & texture, ci::gl::GlslProgRef shader );

		ci::gl::BatchRef		mBatch;
		ci::gl::Texture2dRef	mTexture;
		std::string				mModelName;
	};

	struct VertexDataLens
	{
		glm::vec2 position;
		glm::vec2 texCoordRed;
		glm::vec2 texCoordGreen;
		glm::vec2 texCoordBlue;
	};

	struct FramebufferDesc
	{
		GLuint m_nDepthBufferId;
		GLuint m_nRenderTextureId;
		GLuint m_nRenderFramebufferId;
		GLuint m_nResolveTextureId;
		GLuint m_nResolveFramebufferId;
	};

	typedef std::shared_ptr<class HtcVive> HtcViveRef;

	class HtcVive : ci::Noncopyable
	{
	public:
		static HtcViveRef create() { return HtcViveRef{ new HtcVive }; }
		~HtcVive();
		void update();

		void bind();
		void unbind();

		void renderController( const vr::Hmd_Eye& eye );
		void renderStereoTargets( std::function<void(vr::Hmd_Eye)> renderScene );
		void renderDistortion( const glm::ivec2& windowSize );

		const vr::IVRSystem * getHmd() const { return mHMD; }

		glm::mat4 getHMDMatrixProjectionEye( vr::Hmd_Eye nEye );
		glm::mat4 getHMDMatrixPoseEye( vr::Hmd_Eye nEye );
		glm::mat4 getCurrentViewProjectionMatrix( vr::Hmd_Eye nEye );
		void updateHMDMatrixPose();

		glm::mat4 convertSteamVRMatrixToMat4( const vr::HmdMatrix34_t &matPose );

	private:
		HtcVive();

		void setupShaders();
		void setupStereoRenderTargets();
		void setupDistortion();
		void setupCameras();
		void setupRenderModels();
		void setupRenderModelForTrackedDevice( vr::TrackedDeviceIndex_t unTrackedDeviceIndex );
		void setupCompositor();

		RenderModelRef findOrLoadRenderModel( const std::string& name );

		void processVREvent( const vr::VREvent_t & event );

		std::string m_strPoseClasses;                            // what classes we saw poses for this frame
		char m_rDevClassChar[vr::k_unMaxTrackedDeviceCount];   // for each device, a character representing its class

		float m_fNearClip;
		float m_fFarClip;

		bool mPerf;
		bool mVblank;
		bool mGlFinishHack;

		vr::IVRSystem *			mHMD;
		vr::IVRRenderModels *	m_pRenderModels;
		std::string				mDriver;
		std::string				mDisplay;

		std::array<vr::TrackedDevicePose_t, vr::k_unMaxTrackedDeviceCount> mTrackedDevicePose;
		std::array<glm::mat4, vr::k_unMaxTrackedDeviceCount> mDevicePose;
		std::array<bool, vr::k_unMaxTrackedDeviceCount> mShowTrackedDevice;


		GLuint m_unLensVAO;
		GLuint m_glIDVertBuffer;
		GLuint m_glIDIndexBuffer;
		unsigned int m_uiIndexSize;

		GLuint m_glControllerVertBuffer;
		GLuint m_unControllerVAO;
		unsigned int m_uiControllerVertcount;

		glm::mat4 m_mat4HMDPose;
		glm::mat4 m_mat4eyePosLeft;
		glm::mat4 m_mat4eyePosRight;

		glm::mat4 m_mat4ProjectionCenter;
		glm::mat4 m_mat4ProjectionLeft;
		glm::mat4 m_mat4ProjectionRight;

		ci::gl::GlslProgRef mGlslLens;
		ci::gl::GlslProgRef mGlslModel;

		GLint m_nControllerMatrixLocation;

		int m_iTrackedControllerCount;
		int m_iTrackedControllerCount_Last;
		int m_iValidPoseCount;
		int m_iValidPoseCount_Last;

		FramebufferDesc leftEyeDesc;
		FramebufferDesc rightEyeDesc;
		glm::uvec2 mRenderSize;

		std::vector<RenderModelRef> mRenderModels;
		std::array<RenderModelRef, vr::k_unMaxTrackedDeviceCount> mTrackedDeviceToRenderModel;

	};

	struct ScopedVive {
		ScopedVive( const HtcViveRef& vive );
		~ScopedVive();
	private:
		HtcVive* mVive;
	};

	class ViveExeption : public ci::Exception {
	public:
		ViveExeption() { }
		ViveExeption( const std::string &description ) : Exception( description ) { }
	};

}





