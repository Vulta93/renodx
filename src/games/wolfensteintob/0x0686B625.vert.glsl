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
vec4 swizzleColor ( vec4 color ) { return color; }
uniform vec4 _va_ [8];

layout(location = 2) in vec4 in_Normal;
layout(location = 9) in vec4 in_Tangent;
layout(location = 0) in vec4 in_Position;
layout(location = 1) in vec2 in_TexCoord;
layout(location = 3) in vec4 in_Color;

out vec4 vofi_TexCoord4;
out vec4 vofi_TexCoord1;
out vec4 vofi_TexCoord2;
out vec4 vofi_TexCoord3;
out vec4 vofi_Color;
out vec4 vofi_TexCoord0;

void main() {
	vec4 snormal = in_Normal * 2.0 - 1.0;
	vec4 stangent = in_Tangent * 2.0 - 1.0;
	vec3 normal = normalize( snormal.xyz );
	vec3 tangent = normalize( stangent.xyz - normal * dot( normal, stangent.xyz ) );
	vec3 bitangent = cross( normal, tangent );
	gl_Position.x = dot4( in_Position, _va_[0 ] );
	gl_Position.y = dot4( in_Position, _va_[1 ] );
	gl_Position.z = dot4( in_Position, _va_[2 ] );
	gl_Position.w = dot4( in_Position, _va_[3 ] );
	vec2 scaleBiasX;
	scaleBiasX.x = floor( in_TexCoord.x / _va_[4 ].x );
	scaleBiasX.y = in_TexCoord.x - ( scaleBiasX.x * _va_[4 ].x );
	vec2 scaleBiasY;
	scaleBiasY.x = floor( in_TexCoord.y / _va_[4 ].y );
	scaleBiasY.y = in_TexCoord.y - ( scaleBiasY.x * _va_[4 ].y );
	vofi_TexCoord0.xy = scaleBiasX / _va_[4 ].x;
	vofi_TexCoord0.zw = scaleBiasY / _va_[4 ].y;
	vofi_TexCoord1.xyz = tangent;
	vofi_TexCoord1.w = 0.0;
	vofi_TexCoord2.xyz = bitangent;
	vofi_TexCoord2.w = 0.0;
	vofi_TexCoord3.xyz = normal;
	vofi_TexCoord3.w = 0.0;
	float s = in_Normal.w * 255.1;
	vec3 signs;
	signs.z = floor( s / 4 );
	signs.y = floor( s / 2 ) - ( signs.z * 2 );
	signs.x = s - ( signs.y * 2 ) - ( signs.z * 4 );
	signs = signs * 2.0 - 1.0;
	vec4 col = swizzleColor( in_Color );
	vec3 sizes = col.xyz * 255.0;
	sizes *= signs;
	vec3 origin = in_Position.xyz - sizes;
	vofi_TexCoord4.x = origin.x;
	vofi_TexCoord4.y = origin.y;
	vofi_TexCoord4.z = origin.z;
	vofi_TexCoord4.w = saturate( exp2( -( clamp( gl_Position.w - _va_[5 ].x, 0.0, _va_[6 ].x ) * _va_[7 ].x ) ) );
	vec3 pos = in_Position.xyz - origin;
	vec3 localPos;
	localPos.x = dot3( pos, tangent );
	localPos.y = dot3( pos, bitangent );
	localPos.z = dot3( pos, normal );
	col.xyz = 1.0 / abs( localPos );
	vofi_Color = col;
}