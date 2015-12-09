#version 410

uniform mat4	ciModelViewProjection;

in vec4		ciPosition;
in vec2		ciTexCoord0;
in vec3		vInstancePosition; // per-instance position variable
out highp vec2	TexCoord;

void main( void )
{
	gl_Position	= ciModelViewProjection * ( ciPosition + vec4( vInstancePosition, 0 ) );
	TexCoord	= ciTexCoord0;
}
