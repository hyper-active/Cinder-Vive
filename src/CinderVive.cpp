#include "CinderVive.h"

using namespace ci;
using namespace std;
using namespace hmd;

std::string GetTrackedDeviceString( vr::IVRSystem *pHmd, vr::TrackedDeviceIndex_t unDevice, vr::TrackedDeviceProperty prop, vr::TrackedPropertyError *peError = NULL )
{
	uint32_t unRequiredBufferLen = pHmd->GetStringTrackedDeviceProperty( unDevice, prop, NULL, 0, peError );
	if( unRequiredBufferLen == 0 )
		return "";

	char *pchBuffer = new char[unRequiredBufferLen];
	unRequiredBufferLen = pHmd->GetStringTrackedDeviceProperty( unDevice, prop, pchBuffer, unRequiredBufferLen, peError );
	std::string sResult = pchBuffer;
	delete[] pchBuffer;
	return sResult;
}

RenderModel::RenderModel( const std::string & sRenderModelName, const vr::RenderModel_t & vrModel, const vr::RenderModel_TextureMap_t & vrDiffuseTexture, gl::GlslProgRef shader )
	: mModelName( sRenderModelName )
{
	auto indicesVbo = ci::gl::Vbo::create( GL_ELEMENT_ARRAY_BUFFER, sizeof( uint16_t ) * vrModel.unTriangleCount * 3, vrModel.rIndexData, GL_STATIC_DRAW );
	auto layout = ci::gl::VboMesh::Layout().usage( GL_STATIC_DRAW ) //.interleave( false )
		.attrib( ci::geom::Attrib::POSITION, 3 )
		.attrib( ci::geom::Attrib::NORMAL, 3 )
		.attrib( ci::geom::Attrib::TEX_COORD_0, 2 );
	auto vboMesh = ci::gl::VboMesh::create( vrModel.unVertexCount, GL_TRIANGLES, { layout }, vrModel.unTriangleCount * 3, GL_UNSIGNED_SHORT, indicesVbo );

	ci::Surface8u surface{ const_cast<uint8_t *>(vrDiffuseTexture.rubTextureMapData), vrDiffuseTexture.unWidth, vrDiffuseTexture.unHeight, 4 * vrDiffuseTexture.unWidth, ci::SurfaceChannelOrder::RGBA };
	mTexture = ci::gl::Texture2d::create( surface );

	mBatch = ci::gl::Batch::create( vboMesh, shader );
	mBatch->getGlslProg()->uniform( "diffuse", 0 );
}

void RenderModel::draw()
{
	ci::gl::ScopedTextureBind tex0{ mTexture, 0 };
	mBatch->draw();
}


HtcVive::HtcVive()
	: mHMD( nullptr )
	, m_pRenderModels( nullptr )
	, m_glControllerVertBuffer( 0 )
	, m_unControllerVAO( 0 )
	, m_unLensVAO( 0 )
	, m_nControllerMatrixLocation( -1 )
	, m_iTrackedControllerCount( 0 )
	, m_iTrackedControllerCount_Last( -1 )
	, m_iValidPoseCount( 0 )
	, m_iValidPoseCount_Last( -1 )
{
	memset( m_rDevClassChar, 0, sizeof( m_rDevClassChar ) );

	m_fNearClip = 0.1f;
	m_fFarClip = 37.0f;

	if( gl::isVerticalSyncEnabled() ) {
		CI_LOG_W( "Disabling vertical sync for maximal performance." );
		gl::enableVerticalSync( false );
	}

	// Loading the SteamVR Runtime
	vr::EVRInitError eError = vr::VRInitError_None;
	mHMD = vr::VR_Init( &eError, vr::VRApplication_Scene );

	if( eError != vr::VRInitError_None ) {
		mHMD = nullptr;
		throw ViveExeption{ "Unable to init VR runtime: " + std::string{ vr::VR_GetVRInitErrorAsEnglishDescription( eError ) } };
	}

	m_pRenderModels = (vr::IVRRenderModels *)vr::VR_GetGenericInterface( vr::IVRRenderModels_Version, &eError );
	if( ! m_pRenderModels ) {
		mHMD = nullptr;
		vr::VR_Shutdown();
		throw ViveExeption{ "Unable to get render model interface: " + std::string{ vr::VR_GetVRInitErrorAsEnglishDescription( eError ) } };
	}

	mDriver = GetTrackedDeviceString( mHMD, vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_TrackingSystemName_String );
	mDisplay = GetTrackedDeviceString( mHMD, vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_SerialNumber_String );


	setupShaders();
	setupCameras();
	setupStereoRenderTargets();
	setupDistortion();
	setupRenderModels();
	setupCompositor();
}


HtcVive::~HtcVive()
{
	glDebugMessageControl( GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, nullptr, GL_FALSE );
	glDebugMessageCallback( nullptr, nullptr );
	glDeleteBuffers( 1, &m_glIDVertBuffer );
	glDeleteBuffers( 1, &m_glIDIndexBuffer );

	glDeleteRenderbuffers( 1, &leftEyeDesc.m_nDepthBufferId );
	glDeleteTextures( 1, &leftEyeDesc.m_nRenderTextureId );
	glDeleteFramebuffers( 1, &leftEyeDesc.m_nRenderFramebufferId );
	glDeleteTextures( 1, &leftEyeDesc.m_nResolveTextureId );
	glDeleteFramebuffers( 1, &leftEyeDesc.m_nResolveFramebufferId );

	glDeleteRenderbuffers( 1, &rightEyeDesc.m_nDepthBufferId );
	glDeleteTextures( 1, &rightEyeDesc.m_nRenderTextureId );
	glDeleteFramebuffers( 1, &rightEyeDesc.m_nRenderFramebufferId );
	glDeleteTextures( 1, &rightEyeDesc.m_nResolveTextureId );
	glDeleteFramebuffers( 1, &rightEyeDesc.m_nResolveFramebufferId );

	if( m_unLensVAO != 0 )
	{
		glDeleteVertexArrays( 1, &m_unLensVAO );
	}
	if( m_unControllerVAO != 0 )
	{
		glDeleteVertexArrays( 1, &m_unControllerVAO );
	}

	if( mHMD ) {
		vr::VR_Shutdown();
		mHMD = nullptr;
	}
}

void hmd::HtcVive::update()
{
	vr::VREvent_t event;
	while( mHMD->PollNextEvent( &event ) ) {
		processVREvent( event );
	}

	// Process SteamVR controller state
	for( vr::TrackedDeviceIndex_t unDevice = 0; unDevice < vr::k_unMaxTrackedDeviceCount; unDevice++ ) {
		vr::VRControllerState_t state;
		if( mHMD->GetControllerState( unDevice, &state ) ) {
			mShowTrackedDevice[unDevice] = state.ulButtonPressed == 0;
		}
	}
}

void HtcVive::bind()
{
	updateHMDMatrixPose();
}

void hmd::HtcVive::unbind()
{
	vr::Texture_t leftEyeTexture = { (void*)leftEyeDesc.m_nResolveTextureId, vr::API_OpenGL, vr::ColorSpace_Gamma };
	vr::VRCompositor()->Submit( vr::Eye_Left, &leftEyeTexture );
	vr::Texture_t rightEyeTexture = { (void*)rightEyeDesc.m_nResolveTextureId, vr::API_OpenGL, vr::ColorSpace_Gamma };
	vr::VRCompositor()->Submit( vr::Eye_Right, &rightEyeTexture );

	// Spew out the controller and pose count whenever they change.
	if( m_iTrackedControllerCount != m_iTrackedControllerCount_Last || m_iValidPoseCount != m_iValidPoseCount_Last )
	{
		m_iValidPoseCount_Last = m_iValidPoseCount;
		m_iTrackedControllerCount_Last = m_iTrackedControllerCount;

		//CI_LOG_I( "PoseCount:%d(%s) Controllers:%d\n", m_iValidPoseCount, m_strPoseClasses.c_str(), m_iTrackedControllerCount );
	}
}

void hmd::HtcVive::setupShaders()
{
	mGlslLens = gl::GlslProg::create(
		"#version 410 core\n"
		"layout(location = 0) in vec4 position;\n"
		"layout(location = 1) in vec2 v2UVredIn;\n"
		"layout(location = 2) in vec2 v2UVGreenIn;\n"
		"layout(location = 3) in vec2 v2UVblueIn;\n"
		"noperspective  out vec2 v2UVred;\n"
		"noperspective  out vec2 v2UVgreen;\n"
		"noperspective  out vec2 v2UVblue;\n"
		"void main()\n"
		"{\n"
		"	v2UVred = v2UVredIn;\n"
		"	v2UVgreen = v2UVGreenIn;\n"
		"	v2UVblue = v2UVblueIn;\n"
		"	gl_Position = position;\n"
		"}\n",

		// fragment shader
		"#version 410 core\n"
		"uniform sampler2D mytexture;\n"

		"noperspective  in vec2 v2UVred;\n"
		"noperspective  in vec2 v2UVgreen;\n"
		"noperspective  in vec2 v2UVblue;\n"

		"out vec4 outputColor;\n"

		"void main()\n"
		"{\n"
		"	float fBoundsCheck = ( (dot( vec2( lessThan( v2UVgreen.xy, vec2(0.05, 0.05)) ), vec2(1.0, 1.0))+dot( vec2( greaterThan( v2UVgreen.xy, vec2( 0.95, 0.95)) ), vec2(1.0, 1.0))) );\n"
		"	if( fBoundsCheck > 1.0 )\n"
		"	{ outputColor = vec4( 0, 0, 0, 1.0 ); }\n"
		"	else\n"
		"	{\n"
		"		float red = texture(mytexture, v2UVred).x;\n"
		"		float green = texture(mytexture, v2UVgreen).y;\n"
		"		float blue = texture(mytexture, v2UVblue).z;\n"
		"		outputColor = vec4( red, green, blue, 1.0  ); }\n"
		"}\n"
		);

	mGlslModel = ci::gl::GlslProg::create(
		"#version 410\n"
		"uniform mat4	ciModelViewProjection;\n"
		"in vec4		ciPosition;\n"
		"in vec3		ciNormal;\n"
		"in vec2		ciTexCoord0;\n"
		"out vec2		vTexCoord;\n"
		"void main()\n"
		"{\n"
		"	vTexCoord = ciTexCoord0;\n"
		"	gl_Position = ciModelViewProjection * vec4(ciPosition.xyz, 1);\n"
		"}\n"
		,
		"#version 410\n"
		"uniform sampler2D	diffuse;\n"
		"in vec2			vTexCoord;\n"
		"out vec4			outputColor;\n"
		"void main()\n"
		"{\n"
		"   outputColor = texture( diffuse, vTexCoord );\n"
		"}\n" );
}


bool CreateFrameBuffer( int nWidth, int nHeight, FramebufferDesc &framebufferDesc )
{
	glGenFramebuffers( 1, &framebufferDesc.m_nRenderFramebufferId );
	glBindFramebuffer( GL_FRAMEBUFFER, framebufferDesc.m_nRenderFramebufferId );

	glGenRenderbuffers( 1, &framebufferDesc.m_nDepthBufferId );
	glBindRenderbuffer( GL_RENDERBUFFER, framebufferDesc.m_nDepthBufferId );
	glRenderbufferStorageMultisample( GL_RENDERBUFFER, 4, GL_DEPTH_COMPONENT, nWidth, nHeight );
	glFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, framebufferDesc.m_nDepthBufferId );

	glGenTextures( 1, &framebufferDesc.m_nRenderTextureId );
	glBindTexture( GL_TEXTURE_2D_MULTISAMPLE, framebufferDesc.m_nRenderTextureId );
	glTexImage2DMultisample( GL_TEXTURE_2D_MULTISAMPLE, 4, GL_RGBA8, nWidth, nHeight, true );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_MULTISAMPLE, framebufferDesc.m_nRenderTextureId, 0 );

	glGenFramebuffers( 1, &framebufferDesc.m_nResolveFramebufferId );
	glBindFramebuffer( GL_FRAMEBUFFER, framebufferDesc.m_nResolveFramebufferId );

	glGenTextures( 1, &framebufferDesc.m_nResolveTextureId );
	glBindTexture( GL_TEXTURE_2D, framebufferDesc.m_nResolveTextureId );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0 );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, nWidth, nHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, framebufferDesc.m_nResolveTextureId, 0 );

	// check FBO status
	GLenum status = glCheckFramebufferStatus( GL_FRAMEBUFFER );
	if( status != GL_FRAMEBUFFER_COMPLETE )
	{
		return false;
	}

	glBindFramebuffer( GL_FRAMEBUFFER, 0 );

	return true;
}

void HtcVive::setupStereoRenderTargets()
{
	mHMD->GetRecommendedRenderTargetSize( &mRenderSize.x, &mRenderSize.y );
	CreateFrameBuffer( mRenderSize.x, mRenderSize.y, leftEyeDesc );
	CreateFrameBuffer( mRenderSize.x, mRenderSize.y, rightEyeDesc );
}

void HtcVive::setupDistortion()
{
	GLushort m_iLensGridSegmentCountH = 43;
	GLushort m_iLensGridSegmentCountV = 43;

	float w = (float)(1.0 / float( m_iLensGridSegmentCountH - 1 ));
	float h = (float)(1.0 / float( m_iLensGridSegmentCountV - 1 ));

	float u, v = 0;

	std::vector<VertexDataLens> vVerts( 0 );
	VertexDataLens vert;

	//left eye distortion verts
	float Xoffset = -1;
	for( int y = 0; y<m_iLensGridSegmentCountV; y++ )
	{
		for( int x = 0; x<m_iLensGridSegmentCountH; x++ )
		{
			u = x*w; v = 1 - y*h;
			vert.position = glm::vec2( Xoffset + u, -1 + 2 * y*h );

			vr::DistortionCoordinates_t dc0 = mHMD->ComputeDistortion( vr::Eye_Left, u, v );

			vert.texCoordRed = glm::vec2( dc0.rfRed[0], 1 - dc0.rfRed[1] );
			vert.texCoordGreen = glm::vec2( dc0.rfGreen[0], 1 - dc0.rfGreen[1] );
			vert.texCoordBlue = glm::vec2( dc0.rfBlue[0], 1 - dc0.rfBlue[1] );

			vVerts.push_back( vert );
		}
	}

	//right eye distortion verts
	Xoffset = 0;
	for( int y = 0; y<m_iLensGridSegmentCountV; y++ )
	{
		for( int x = 0; x<m_iLensGridSegmentCountH; x++ )
		{
			u = x*w; v = 1 - y*h;
			vert.position = glm::vec2( Xoffset + u, -1 + 2 * y*h );

			vr::DistortionCoordinates_t dc0 = mHMD->ComputeDistortion( vr::Eye_Right, u, v );

			vert.texCoordRed = glm::vec2( dc0.rfRed[0], 1 - dc0.rfRed[1] );
			vert.texCoordGreen = glm::vec2( dc0.rfGreen[0], 1 - dc0.rfGreen[1] );
			vert.texCoordBlue = glm::vec2( dc0.rfBlue[0], 1 - dc0.rfBlue[1] );

			vVerts.push_back( vert );
		}
	}

	std::vector<GLushort> vIndices;
	GLushort a, b, c, d;

	GLushort offset = 0;
	for( GLushort y = 0; y<m_iLensGridSegmentCountV - 1; y++ )
	{
		for( GLushort x = 0; x<m_iLensGridSegmentCountH - 1; x++ )
		{
			a = m_iLensGridSegmentCountH*y + x + offset;
			b = m_iLensGridSegmentCountH*y + x + 1 + offset;
			c = (y + 1)*m_iLensGridSegmentCountH + x + 1 + offset;
			d = (y + 1)*m_iLensGridSegmentCountH + x + offset;
			vIndices.push_back( a );
			vIndices.push_back( b );
			vIndices.push_back( c );

			vIndices.push_back( a );
			vIndices.push_back( c );
			vIndices.push_back( d );
		}
	}

	offset = (m_iLensGridSegmentCountH)*(m_iLensGridSegmentCountV);
	for( GLushort y = 0; y<m_iLensGridSegmentCountV - 1; y++ )
	{
		for( GLushort x = 0; x<m_iLensGridSegmentCountH - 1; x++ )
		{
			a = m_iLensGridSegmentCountH*y + x + offset;
			b = m_iLensGridSegmentCountH*y + x + 1 + offset;
			c = (y + 1)*m_iLensGridSegmentCountH + x + 1 + offset;
			d = (y + 1)*m_iLensGridSegmentCountH + x + offset;
			vIndices.push_back( a );
			vIndices.push_back( b );
			vIndices.push_back( c );

			vIndices.push_back( a );
			vIndices.push_back( c );
			vIndices.push_back( d );
		}
	}
	m_uiIndexSize = vIndices.size();

	glGenVertexArrays( 1, &m_unLensVAO );
	glBindVertexArray( m_unLensVAO );

	glGenBuffers( 1, &m_glIDVertBuffer );
	glBindBuffer( GL_ARRAY_BUFFER, m_glIDVertBuffer );
	glBufferData( GL_ARRAY_BUFFER, vVerts.size()*sizeof( VertexDataLens ), &vVerts[0], GL_STATIC_DRAW );

	glGenBuffers( 1, &m_glIDIndexBuffer );
	glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, m_glIDIndexBuffer );
	glBufferData( GL_ELEMENT_ARRAY_BUFFER, vIndices.size()*sizeof( GLushort ), &vIndices[0], GL_STATIC_DRAW );

	glEnableVertexAttribArray( 0 );
	glVertexAttribPointer( 0, 2, GL_FLOAT, GL_FALSE, sizeof( VertexDataLens ), (void *)offsetof( VertexDataLens, position ) );

	glEnableVertexAttribArray( 1 );
	glVertexAttribPointer( 1, 2, GL_FLOAT, GL_FALSE, sizeof( VertexDataLens ), (void *)offsetof( VertexDataLens, texCoordRed ) );

	glEnableVertexAttribArray( 2 );
	glVertexAttribPointer( 2, 2, GL_FLOAT, GL_FALSE, sizeof( VertexDataLens ), (void *)offsetof( VertexDataLens, texCoordGreen ) );

	glEnableVertexAttribArray( 3 );
	glVertexAttribPointer( 3, 2, GL_FLOAT, GL_FALSE, sizeof( VertexDataLens ), (void *)offsetof( VertexDataLens, texCoordBlue ) );

	glBindVertexArray( 0 );

	glDisableVertexAttribArray( 0 );
	glDisableVertexAttribArray( 1 );
	glDisableVertexAttribArray( 2 );
	glDisableVertexAttribArray( 3 );

	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, 0 );
}

void HtcVive::setupCameras()
{
	m_mat4ProjectionLeft = getHMDMatrixProjectionEye( vr::Eye_Left );
	m_mat4ProjectionRight = getHMDMatrixProjectionEye( vr::Eye_Right );
	m_mat4eyePosLeft = getHMDMatrixPoseEye( vr::Eye_Left );
	m_mat4eyePosRight = getHMDMatrixPoseEye( vr::Eye_Right );
}

void HtcVive::setupRenderModels()
{
	for( auto id = vr::k_unTrackedDeviceIndex_Hmd + 1; id < vr::k_unMaxTrackedDeviceCount; id++ ) {
		if( !mHMD->IsTrackedDeviceConnected( id ) )
			continue;

		setupRenderModelForTrackedDevice( id );
	}
}

void HtcVive::setupRenderModelForTrackedDevice( vr::TrackedDeviceIndex_t unTrackedDeviceIndex )
{
	if( unTrackedDeviceIndex >= vr::k_unMaxTrackedDeviceCount )
		return;

	// try to find a model we've already set up
	std::string sRenderModelName = GetTrackedDeviceString( mHMD, unTrackedDeviceIndex, vr::Prop_RenderModelName_String );
	auto renderModel = findOrLoadRenderModel( sRenderModelName );
	if( !renderModel ) {
		std::string sTrackingSystemName = GetTrackedDeviceString( mHMD, unTrackedDeviceIndex, vr::Prop_TrackingSystemName_String );
		CI_LOG_E( "Unable to load render model for tracked device " << unTrackedDeviceIndex << " " << sTrackingSystemName << " " << sRenderModelName );
	}
	else {
		mTrackedDeviceToRenderModel[unTrackedDeviceIndex] = renderModel;
		mShowTrackedDevice[unTrackedDeviceIndex] = true;
	}
}

void hmd::HtcVive::setupCompositor()
{
	if( !vr::VRCompositor() ) {
		throw ViveExeption{ "Compositor initialization failed. See log file for details." };
	}
}

void hmd::HtcVive::renderController( const vr::Hmd_Eye& eye )
{
	bool inputCapturedByAnotherProcess = mHMD->IsInputFocusCapturedByAnotherProcess();

	for( uint32_t i = 0; i < vr::k_unMaxTrackedDeviceCount; i++ )
	{
		if( /* ! mTrackedDeviceToRenderModel[i] || */ ! mShowTrackedDevice[i] )
			continue;

		const vr::TrackedDevicePose_t & pose = mTrackedDevicePose[i];
		if( !pose.bPoseIsValid )
			continue;

		if( inputCapturedByAnotherProcess && mHMD->GetTrackedDeviceClass( i ) == vr::TrackedDeviceClass_Controller )
			continue;
		{
			gl::ScopedModelMatrix push;
			gl::setModelMatrix( mDevicePose[i] );
			gl::drawCoordinateFrame( 0.3f, 0.06f, 0.01f );
		}

		//mTrackedDeviceToRenderModel[i]->draw(); //TODO: Fix render!
	}
}

void HtcVive::processVREvent( const vr::VREvent_t & event )
{
	switch( event.eventType ) {
	case vr::VREvent_TrackedDeviceActivated:
	{
		CI_LOG_I( "Device " << event.trackedDeviceIndex << " attached. Setting up render model." );
		setupRenderModelForTrackedDevice( event.trackedDeviceIndex );
	}
	break;
	case vr::VREvent_TrackedDeviceDeactivated:
	{
		CI_LOG_I( "Device " << event.trackedDeviceIndex << " detached." );
	}
	break;
	case vr::VREvent_TrackedDeviceUpdated:
	{
		CI_LOG_I( "Device " << event.trackedDeviceIndex << " updated." );
	}
	break;
	}
}

void hmd::HtcVive::renderStereoTargets( std::function<void( vr::Hmd_Eye )> renderScene )
{
	glEnable( GL_MULTISAMPLE );

	// Left Eye
	glBindFramebuffer( GL_FRAMEBUFFER, leftEyeDesc.m_nRenderFramebufferId );
	glViewport( 0, 0, mRenderSize.x, mRenderSize.y );
	{
		gl::ScopedViewMatrix pushView;
		gl::ScopedProjectionMatrix pushProj;
		gl::setViewMatrix( m_mat4eyePosLeft * m_mat4HMDPose );
		gl::setProjectionMatrix( m_mat4ProjectionLeft );
		renderScene( vr::Eye_Left );
		renderController( vr::Eye_Left );
	}
	glBindFramebuffer( GL_FRAMEBUFFER, 0 );

	glDisable( GL_MULTISAMPLE );

	glBindFramebuffer( GL_READ_FRAMEBUFFER, leftEyeDesc.m_nRenderFramebufferId );
	glBindFramebuffer( GL_DRAW_FRAMEBUFFER, leftEyeDesc.m_nResolveFramebufferId );

	glBlitFramebuffer( 0, 0, mRenderSize.x, mRenderSize.y, 0, 0, mRenderSize.x, mRenderSize.y,
		GL_COLOR_BUFFER_BIT,
		GL_LINEAR );

	glBindFramebuffer( GL_READ_FRAMEBUFFER, 0 );
	glBindFramebuffer( GL_DRAW_FRAMEBUFFER, 0 );

	glEnable( GL_MULTISAMPLE );

	// Right Eye
	glBindFramebuffer( GL_FRAMEBUFFER, rightEyeDesc.m_nRenderFramebufferId );
	glViewport( 0, 0, mRenderSize.x, mRenderSize.y );
	{
		gl::ScopedViewMatrix pushView;
		gl::ScopedProjectionMatrix pushProj;
		gl::setViewMatrix( m_mat4eyePosRight * m_mat4HMDPose );
		gl::setProjectionMatrix( m_mat4ProjectionRight );
		renderScene( vr::Eye_Right );
		renderController( vr::Eye_Right );
	}
	glBindFramebuffer( GL_FRAMEBUFFER, 0 );

	glDisable( GL_MULTISAMPLE );

	glBindFramebuffer( GL_READ_FRAMEBUFFER, rightEyeDesc.m_nRenderFramebufferId );
	glBindFramebuffer( GL_DRAW_FRAMEBUFFER, rightEyeDesc.m_nResolveFramebufferId );

	glBlitFramebuffer( 0, 0, mRenderSize.x, mRenderSize.y, 0, 0, mRenderSize.x, mRenderSize.y,
		GL_COLOR_BUFFER_BIT,
		GL_LINEAR );

	glBindFramebuffer( GL_READ_FRAMEBUFFER, 0 );
	glBindFramebuffer( GL_DRAW_FRAMEBUFFER, 0 );
}

void HtcVive::renderDistortion( const ivec2& windowSize )
{
	glDisable( GL_DEPTH_TEST );
	glViewport( 0, 0, windowSize.x, windowSize.y );

	glBindVertexArray( m_unLensVAO );
	glUseProgram( mGlslLens->getHandle() );

	//render left lens (first half of index array )
	glBindTexture( GL_TEXTURE_2D, leftEyeDesc.m_nResolveTextureId );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR );
	glDrawElements( GL_TRIANGLES, m_uiIndexSize / 2, GL_UNSIGNED_SHORT, 0 );

	//render right lens (second half of index array )
	glBindTexture( GL_TEXTURE_2D, rightEyeDesc.m_nResolveTextureId );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR );
	glDrawElements( GL_TRIANGLES, m_uiIndexSize / 2, GL_UNSIGNED_SHORT, (const void *)(m_uiIndexSize) );

	glBindVertexArray( 0 );
	glUseProgram( 0 );
}

glm::mat4 HtcVive::getHMDMatrixProjectionEye( vr::Hmd_Eye nEye )
{
	if( ! mHMD )
		return glm::mat4();

	auto mat = mHMD->GetProjectionMatrix( nEye, m_fNearClip, m_fFarClip, vr::API_OpenGL );

	return glm::mat4(
		mat.m[0][0], mat.m[1][0], mat.m[2][0], mat.m[3][0],
		mat.m[0][1], mat.m[1][1], mat.m[2][1], mat.m[3][1],
		mat.m[0][2], mat.m[1][2], mat.m[2][2], mat.m[3][2],
		mat.m[0][3], mat.m[1][3], mat.m[2][3], mat.m[3][3]
		);
}

glm::mat4 HtcVive::getHMDMatrixPoseEye( vr::Hmd_Eye nEye )
{
	if( ! mHMD )
		return glm::mat4();

	vr::HmdMatrix34_t matEyeRight = mHMD->GetEyeToHeadTransform( nEye );
	glm::mat4 matrixObj(
		matEyeRight.m[0][0], matEyeRight.m[1][0], matEyeRight.m[2][0], 0.0,
		matEyeRight.m[0][1], matEyeRight.m[1][1], matEyeRight.m[2][1], 0.0,
		matEyeRight.m[0][2], matEyeRight.m[1][2], matEyeRight.m[2][2], 0.0,
		matEyeRight.m[0][3], matEyeRight.m[1][3], matEyeRight.m[2][3], 1.0f
		);

	return glm::inverse( matrixObj );
}

glm::mat4 HtcVive::getCurrentViewProjectionMatrix( vr::Hmd_Eye nEye )
{
	glm::mat4 matMVP;
	if( nEye == vr::Eye_Left )
	{
		matMVP = m_mat4ProjectionLeft * m_mat4eyePosLeft * m_mat4HMDPose;
	}
	else if( nEye == vr::Eye_Right )
	{
		matMVP = m_mat4ProjectionRight * m_mat4eyePosRight *  m_mat4HMDPose;
	}

	return matMVP;
}

void HtcVive::updateHMDMatrixPose()
{
	vr::VRCompositor()->WaitGetPoses( mTrackedDevicePose.data(), vr::k_unMaxTrackedDeviceCount, NULL, 0 );

	m_iValidPoseCount = 0;
	m_strPoseClasses = "";
	for( int nDevice = 0; nDevice < vr::k_unMaxTrackedDeviceCount; ++nDevice )
	{
		if( mTrackedDevicePose[nDevice].bPoseIsValid )
		{
			m_iValidPoseCount++;
			mDevicePose[nDevice] = convertSteamVRMatrixToMat4( mTrackedDevicePose[nDevice].mDeviceToAbsoluteTracking );
			if( m_rDevClassChar[nDevice] == 0 )
			{
				switch( mHMD->GetTrackedDeviceClass( nDevice ) )
				{
				case vr::TrackedDeviceClass_Controller:        m_rDevClassChar[nDevice] = 'C'; break;
				case vr::TrackedDeviceClass_HMD:               m_rDevClassChar[nDevice] = 'H'; break;
				case vr::TrackedDeviceClass_Invalid:           m_rDevClassChar[nDevice] = 'I'; break;
				case vr::TrackedDeviceClass_Other:             m_rDevClassChar[nDevice] = 'O'; break;
				case vr::TrackedDeviceClass_TrackingReference: m_rDevClassChar[nDevice] = 'T'; break;
				default:                                       m_rDevClassChar[nDevice] = '?'; break;
				}
			}
			m_strPoseClasses += m_rDevClassChar[nDevice];
		}
	}

	if( mTrackedDevicePose[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid )
	{
		m_mat4HMDPose = glm::inverse( mDevicePose[vr::k_unTrackedDeviceIndex_Hmd] );
	}
}

RenderModelRef HtcVive::findOrLoadRenderModel( const std::string& name )
{
	auto resIt = std::find_if( std::begin( mRenderModels ), std::end( mRenderModels ), [&]( const RenderModelRef& m ) {
		return m->GetName() == name;
	} );

	// load the model if we didn't find one
	if( resIt == std::end( mRenderModels ) ) {
		vr::RenderModel_t *pModel = NULL;
		if( !vr::VRRenderModels()->LoadRenderModel( name.c_str(), &pModel ) || pModel == NULL ) {
			CI_LOG_E( "Unable to load render model " << name );
			return nullptr; // move on to the next tracked device
		}

		vr::RenderModel_TextureMap_t *pTexture = NULL;
		if( !vr::VRRenderModels()->LoadTexture( pModel->diffuseTextureId, &pTexture ) || pTexture == NULL ) {
			CI_LOG_E( "Unable to load render texture id:%d for render model %s\n", pModel->diffuseTextureId, pchRenderModelName );
			vr::VRRenderModels()->FreeRenderModel( pModel );
			return nullptr; // move on to the next tracked device
		}

		auto model = RenderModel::create( name, *pModel, *pTexture, mGlslModel );
		mRenderModels.emplace_back( model );

		vr::VRRenderModels()->FreeRenderModel( pModel );
		vr::VRRenderModels()->FreeTexture( pTexture );

		return model;
	}
	return nullptr;
}


glm::mat4 HtcVive::convertSteamVRMatrixToMat4( const vr::HmdMatrix34_t &matPose )
{
	glm::mat4 matrixObj(
		matPose.m[0][0], matPose.m[1][0], matPose.m[2][0], 0.0,
		matPose.m[0][1], matPose.m[1][1], matPose.m[2][1], 0.0,
		matPose.m[0][2], matPose.m[1][2], matPose.m[2][2], 0.0,
		matPose.m[0][3], matPose.m[1][3], matPose.m[2][3], 1.0f
		);
	return matrixObj;
}

hmd::ScopedVive::ScopedVive( const HtcViveRef & vive )
	: mVive( vive.get() )
{
	mVive->bind();
}

hmd::ScopedVive::~ScopedVive()
{
	mVive->unbind();
}
