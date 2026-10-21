#version 150
// RENODX: replacement programs are linked by ReShade without the game's glBindAttribLocation / glBindFragDataLocation
// calls, so every input and output location is pinned here to the game's fixed convention (logged from the original
// programs): in_Position 0, in_TexCoord 1, in_Normal 2, in_Color 3, in_TexCoord1 4, in_Position1 5, in_Tangent 9,
// in_Morph 10; out_FragColorN = N.
#extension GL_ARB_explicit_attrib_location : require

float saturate( float v ) { return clamp( v, 0.0, 1.0 ); }
vec2 saturate( vec2 v ) { return clamp( v, 0.0, 1.0 ); }
vec3 saturate( vec3 v ) { return clamp( v, 0.0, 1.0 ); }
vec4 saturate( vec4 v ) { return clamp( v, 0.0, 1.0 ); }
vec4 tex2Dlod( sampler2D sampler, vec4 texcoord ) { return textureLod( sampler, texcoord.xy, texcoord.w ); }

float dot3 ( vec3 a, vec3 b ) { return dot( a, b ); }
float dot3 ( vec3 a, vec4 b ) { return dot( a, b.xyz ); }
float dot3 ( vec4 a, vec3 b ) { return dot( a.xyz, b ); }
float dot3 ( vec4 a, vec4 b ) { return dot( a.xyz, b.xyz ); }
float dot4 ( vec4 a, vec4 b ) { return dot( a, b ); }
float dot4 ( vec2 a, vec4 b ) { return dot( vec4( a, 0.0, 1.0 ), b ); }
uniform vec4 _va_ [11];

layout(location = 0) in vec4 in_Position;
layout(location = 2) in vec4 in_Normal;
layout(location = 9) in vec4 in_Tangent;
layout(location = 1) in vec2 in_TexCoord;

out vec4 vofi_TexCoord0;
out vec4 vofi_TexCoord6;
out vec4 vofi_TexCoord1;
out vec4 vofi_TexCoord2;
out vec4 vofi_TexCoord3;
out vec4 vofi_TexCoord4;

void main() {
	vec4 position;
	vec4 normal;
	vec4 tangent;
	{
		position = in_Position * _va_[0 ] + _va_[1 ];
		normal = in_Normal * 2.0 - 1.0;
		tangent.xyz = in_Tangent.xyz * 2.0 - 1.0;
		tangent.w = floor( in_Tangent.w * 255.1 / 128.0 ) * 2.0 - 1.0;
	};
	gl_Position.x = dot( position, _va_[2 ] );
	gl_Position.y = dot( position, _va_[3 ] );
	gl_Position.z = dot( position, _va_[4 ] );
	gl_Position.w = dot( position, _va_[5 ] );
	vec3 bitangent = cross( normal.xyz, tangent.xyz ) * tangent.w;
	vofi_TexCoord0 = ( in_TexCoord.xy * _va_[6 ].xy + _va_[6 ].zw ).xyxy;
	vofi_TexCoord1.x = dot3( tangent, _va_[7 ] );
	vofi_TexCoord1.y = dot3( bitangent, _va_[7 ] );
	vofi_TexCoord1.z = dot3( normal, _va_[7 ] );
	vofi_TexCoord1.w = 0.0;
	vofi_TexCoord2.x = dot3( tangent, _va_[8 ] );
	vofi_TexCoord2.y = dot3( bitangent, _va_[8 ] );
	vofi_TexCoord2.z = dot3( normal, _va_[8 ] );
	vofi_TexCoord2.w = 0.0;
	vofi_TexCoord3.x = dot3( tangent, _va_[9 ] );
	vofi_TexCoord3.y = dot3( bitangent, _va_[9 ] );
	vofi_TexCoord3.z = dot3( normal, _va_[9 ] );
	vofi_TexCoord3.w = 0.0;
	vofi_TexCoord4.x = dot4( position, _va_[7 ] );
	vofi_TexCoord4.y = dot4( position, _va_[8 ] );
	vofi_TexCoord4.z = dot4( position, _va_[9 ] );
	vofi_TexCoord4.xyz -= _va_[10 ].xyz;
	vofi_TexCoord4.w = 0.0;
	vofi_TexCoord6.x = dot3( position, _va_[7 ] );
	vofi_TexCoord6.y = dot3( position, _va_[8 ] );
	vofi_TexCoord6.z = dot3( position, _va_[9 ] );
	vofi_TexCoord6.w = 0.0;
}