#include "cinder/app/App.h"
#include "cinder/app/RendererGl.h"
#include "cinder/gl/gl.h"

#include "CinderVive.h"

using namespace ci;
using namespace ci::app;
using namespace std;
using namespace hmd;

class HelloVrApp : public App {
public:
	HelloVrApp();

	void update() override;
	void draw() override;
	void keyDown( KeyEvent event ) override;

	void finishDraw();

	void setupScene();
	void renderScene( vr::Hmd_Eye eye );
private:
	hmd::HtcViveRef		mVive;

	gl::Texture2dRef	mCubeTexture;
	gl::BatchRef		mCubeBatch;
	gl::GlslProgRef		mCubeGlsl;
};

HelloVrApp::HelloVrApp()
{
	auto rgl = static_cast<RendererGl *>(getWindow()->getRenderer().get());
	rgl->setFinishDrawFn( std::bind( &HelloVrApp::finishDraw, this ) );

	setupScene();

	try {
		mVive = hmd::HtcVive::create();
	}
	catch( const hmd::ViveExeption& exc ) {
		CI_LOG_E( exc.what() );
	}
}

void HelloVrApp::setupScene()
{
	GLfloat fLargest;
	glGetFloatv( GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &fLargest );
	gl::Texture2d::Format fmt;
	fmt.setMaxAnisotropy( fLargest );
	fmt.mipmap( true );
	fmt.loadTopDown();
	mCubeTexture = gl::Texture2d::create( loadImage( loadAsset( "cube_texture.png" ) ), fmt );
	auto cubeMesh = gl::VboMesh::create( geom::Cube().size( vec3(0.5) ) );

	mCubeGlsl = gl::GlslProg::create( gl::GlslProg::Format().vertex( loadAsset( "cube.vert" ) ).fragment( loadAsset( "cube.frag" ) ) );
	mCubeGlsl->uniform( "uTex0", 0 );

	// create an array of initial per-instance positions laid out in a 2D grid
	float spacing = 2.0f;
	std::vector<vec3> positions;
	for( int z = -10; z <= 10; z++ ) {
		for( int y = -10; y <= 10; y++ ) {
			for( int x = -10; x <= 10; x++ ) {
				positions.emplace_back( vec3( spacing * x, spacing * y, spacing * z ) );
			}
		}
	}
	auto instanceDataVbo = gl::Vbo::create( GL_ARRAY_BUFFER, positions.size() * sizeof( vec3 ), positions.data(), GL_STATIC_DRAW );
	geom::BufferLayout instanceDataLayout;
	instanceDataLayout.append( geom::Attrib::CUSTOM_0, 3, 0, 0, 1 /* per instance */ );
	cubeMesh->appendVbo( instanceDataLayout, instanceDataVbo );

	mCubeBatch = gl::Batch::create( cubeMesh, mCubeGlsl, { { geom::Attrib::CUSTOM_0, "vInstancePosition" } } );
}

void HelloVrApp::renderScene( vr::Hmd_Eye eye )
{
	glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
	glEnable( GL_DEPTH_TEST );

	gl::ScopedTextureBind tex0{ mCubeTexture, 0 };
	mCubeBatch->drawInstanced( 21 * 21 * 21 );
}


void HelloVrApp::update()
{	
	if( mVive )
		mVive->update();
}

void HelloVrApp::draw()
{
	if( mVive ) {
		hmd::ScopedVive bind{ mVive };
		mVive->renderStereoTargets( std::bind( &HelloVrApp::renderScene, this, std::placeholders::_1 ) );
		mVive->renderDistortion( app::getWindowSize() );
	}
}

void HelloVrApp::finishDraw()
{
	auto rgl = static_cast<RendererGl *>(getWindow()->getRenderer().get());
	rgl->swapBuffers();

	glClearColor( 0, 0, 0, 1 );
	glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
}

void HelloVrApp::keyDown( KeyEvent event )
{
	if( event.getCode() == KeyEvent::KEY_ESCAPE ) {
		quit();
	}
}

void prepareSettings( App::Settings* settings )
{
	settings->setWindowSize( 1280, 720 );
	settings->disableFrameRate();
}

CINDER_APP( HelloVrApp, RendererGl( RendererGl::Options().msaa( 16 ) ), prepareSettings )
