#version 410

uniform sampler2D uTex0;

in vec2		TexCoord;
out vec4	oColor;

void main( void )
{
	oColor = texture( uTex0, TexCoord.st );
}