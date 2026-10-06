#version 150
// RENODX: the replacement program is linked without the game's output-location setup, so with 3 outputs the linker
// may assign them in any order (seen: scene colour and VT feedback swapped). Pin them to the draw-buffer order
// observed in the Devkit (RT0 colour, RT1 VT feedback, RT2 normal + specular).
#extension GL_ARB_explicit_attrib_location : require

void clip( float v ) { if ( v < 0.0 ) { discard; } }
void clip( vec2 v ) { if ( any( lessThan( v, vec2( 0.0 ) ) ) ) { discard; } }
void clip( vec3 v ) { if ( any( lessThan( v, vec3( 0.0 ) ) ) ) { discard; } }
void clip( vec4 v ) { if ( any( lessThan( v, vec4( 0.0 ) ) ) ) { discard; } }

float saturate( float v ) { return clamp( v, 0.0, 1.0 ); }
vec2 saturate( vec2 v ) { return clamp( v, 0.0, 1.0 ); }
vec3 saturate( vec3 v ) { return clamp( v, 0.0, 1.0 ); }
vec4 saturate( vec4 v ) { return clamp( v, 0.0, 1.0 ); }

vec4 tex2D( sampler2D sampler, vec2 texcoord ) { return texture( sampler, texcoord.xy ); }
vec4 tex2D( sampler2DShadow sampler, vec3 texcoord ) { return vec4( texture( sampler, texcoord.xyz ) ); }

vec4 tex2D( sampler2D sampler, vec2 texcoord, vec2 dx, vec2 dy ) { return textureGrad( sampler, texcoord.xy, dx, dy ); }
vec4 tex2D( sampler2DShadow sampler, vec3 texcoord, vec2 dx, vec2 dy ) { return vec4( textureGrad( sampler, texcoord.xyz, dx, dy ) ); }

vec4 texCUBE( samplerCube sampler, vec3 texcoord ) { return texture( sampler, texcoord.xyz ); }
vec4 texCUBE( samplerCubeShadow sampler, vec4 texcoord ) { return vec4( texture( sampler, texcoord.xyzw ) ); }

vec4 tex1Dproj( sampler1D sampler, vec2 texcoord ) { return textureProj( sampler, texcoord ); }
vec4 tex2Dproj( sampler2D sampler, vec3 texcoord ) { return textureProj( sampler, texcoord ); }
vec4 tex3Dproj( sampler3D sampler, vec4 texcoord ) { return textureProj( sampler, texcoord ); }

vec4 tex1Dbias( sampler1D sampler, vec4 texcoord ) { return texture( sampler, texcoord.x, texcoord.w ); }
vec4 tex2Dbias( sampler2D sampler, vec4 texcoord ) { return texture( sampler, texcoord.xy, texcoord.w ); }
vec4 tex3Dbias( sampler3D sampler, vec4 texcoord ) { return texture( sampler, texcoord.xyz, texcoord.w ); }
vec4 texCUBEbias( samplerCube sampler, vec4 texcoord ) { return texture( sampler, texcoord.xyz, texcoord.w ); }

vec4 tex1Dlod( sampler1D sampler, vec4 texcoord ) { return textureLod( sampler, texcoord.x, texcoord.w ); }
vec4 tex2Dlod( sampler2D sampler, vec4 texcoord ) { return textureLod( sampler, texcoord.xy, texcoord.w ); }
vec4 tex3Dlod( sampler3D sampler, vec4 texcoord ) { return textureLod( sampler, texcoord.xyz, texcoord.w ); }
vec4 texCUBElod( samplerCube sampler, vec4 texcoord ) { return textureLod( sampler, texcoord.xyz, texcoord.w ); }

float dot2 ( vec2 a, vec2 b ) { return dot( a, b ); }
float dot3 ( vec3 a, vec3 b ) { return dot( a, b ); }
float dot3 ( vec3 a, vec4 b ) { return dot( a, b.xyz ); }
float dot3 ( vec4 a, vec3 b ) { return dot( a.xyz, b ); }
float dot3 ( vec4 a, vec4 b ) { return dot( a.xyz, b.xyz ); }
float dot4 ( vec4 a, vec4 b ) { return dot( a, b ); }
float dot4 ( vec2 a, vec4 b ) { return dot( vec4( a, 0.0, 1.0 ), b ); }
vec3 pow3 ( vec3 a, float b ) { return vec3( pow( a.x, b ), pow( a.y, b ), pow( a.z, b ) ); }
vec4 h4texPhys ( sampler2D image, vec2 texcoord ) { return tex2D( image, texcoord ); }
vec4 h4texPhys ( sampler2D image, vec2 texcoord, vec2 dx, vec2 dy ) { return tex2D( image, texcoord, dx, dy ); }
const vec4 matrixCoCg1YtoRGB1X = vec4( 1.0, -1.0, 0.0, 1.0 );
const vec4 matrixCoCg1YtoRGB1Y = vec4( 0.0, 1.0, -0.50196078, 1.0 );
const vec4 matrixCoCg1YtoRGB1Z = vec4( -1.0, -1.0, 1.00392156, 1.0 );
uniform vec4 _fa_ [16];
uniform sampler2D samp_minlodmap;
uniform sampler2D samp_pagetablemap;
uniform sampler2D samp_physicalmappingsmap;
uniform sampler2D samp_physicalpagesmap0;
uniform sampler2D samp_physicalpagesmap1;
uniform sampler2D samp_physicalpagesmap2;
uniform samplerCube samp_dynamicenvmap;

in vec4 vofi_TexCoord0;
in vec4 vofi_TexCoord6;
in vec4 vofi_TexCoord1;
in vec4 vofi_TexCoord2;
in vec4 vofi_TexCoord3;
in vec4 vofi_TexCoord4;

layout(location = 0) out vec4 out_FragColor0;
layout(location = 1) out vec4 out_FragColor1;
layout(location = 2) out vec4 out_FragColor2;

void main() {
	vec4 sampleSpecular;
	vec4 sampleYCoCg;
	vec4 sampleNormal;
	vec4 feedback;
	vec4 specular = vec4( 0 );
	vec3 globalNormal = vec3( 0 );
	vec4 color;
	{
		float anisoLOD;
		float sampleLOD;
		{
			float widthInTexels = _fa_[0 ].z;
			float maxAnisoLog2 = _fa_[1 ].z;
			vec2 texelCoords = vofi_TexCoord0.xy * widthInTexels;
			vec2 dx = dFdx( texelCoords );
			vec2 dy = dFdy( texelCoords );
			float px = dot2( dx, dx );
			float py = dot2( dy, dy );
			float maxLod = 0.0;
			float minLod = 0.0;
			if ( px > 0.0 && py > 0.0 ) {
				maxLod = 0.5 * log2( max( px, py ) );
				minLod = 0.5 * log2( min( px, py ) );
			}
			anisoLOD = maxLod - min( maxAnisoLog2, maxLod - minLod );
		};
		{
			sampleLOD = anisoLOD;
			if ( _fa_[2 ].x != 0.0 ) {
				float minLod = tex2D( samp_minlodmap, vofi_TexCoord0.xy ).x * 16 - ( 0.49 + _fa_[3 ].x );
				sampleLOD = max( anisoLOD, minLod );
			}
		};
		{
			float pageSource = _fa_[0 ].x;
			float widthInPages = _fa_[0 ].y;
			float feedbackBias = _fa_[1 ].y;
			feedback.xy = vofi_TexCoord0.xy * widthInPages;
			feedback.z = max( 0.0, anisoLOD ) + feedbackBias;
			feedback.w = pageSource;
		};
		{
			feedback = floor( feedback ) / 256.0;
			vec2 xy_low = fract( feedback.xy + 0.5 / 256.0 );
			vec2 xy_high = floor( feedback.xy ) / 256.0;
			vec4 pack;
			pack.xy = xy_low;
			pack.z = xy_high.y * 16.0 + 0.5 / 256 + xy_high.x;
			pack.w = feedback.w * 16.0 + 0.5 / 256 + feedback.z;
			feedback = pack;
		};
		{
			vec3 physCoords;
			{
				vec4 virtCoordsLod = vec4( vofi_TexCoord0.xy.x, vofi_TexCoord0.xy.y, 0, sampleLOD - 0.5 );
				vec2 physPage = tex2Dlod( samp_pagetablemap, virtCoordsLod ).xy;
				vec4 xform = tex2D( samp_physicalmappingsmap, physPage );
				physCoords.xy = vofi_TexCoord0.xy * xform.x + xform.zw;
				physCoords.z = xform.y;
			};
			{
				sampleSpecular = h4texPhys( samp_physicalpagesmap0, physCoords.xy );
				sampleYCoCg = h4texPhys( samp_physicalpagesmap1, physCoords.xy );
				sampleNormal = h4texPhys( samp_physicalpagesmap2, physCoords.xy );
			};
			if ( _fa_[2 ].y != 0.0 ) {
				vec4 sampleSpecular2, sampleYCoCg2, sampleNormal2;
				{
					vec4 virtCoordsLod = vec4( vofi_TexCoord0.xy.x, vofi_TexCoord0.xy.y, 0, sampleLOD + 0.5 );
					vec2 physPage = tex2Dlod( samp_pagetablemap, virtCoordsLod ).xy;
					vec4 xform = tex2D( samp_physicalmappingsmap, physPage );
					physCoords.xy = vofi_TexCoord0.xy * xform.x + xform.zw;
					physCoords.z = xform.y;
				};
				{
					sampleSpecular2 = h4texPhys( samp_physicalpagesmap0, physCoords.xy );
					sampleYCoCg2 = h4texPhys( samp_physicalpagesmap1, physCoords.xy );
					sampleNormal2 = h4texPhys( samp_physicalpagesmap2, physCoords.xy );
				};
				float trilinearFraction = fract( sampleLOD + _fa_[2 ].z );
				sampleSpecular = mix( sampleSpecular, sampleSpecular2, trilinearFraction );
				sampleYCoCg = mix( sampleYCoCg, sampleYCoCg2, trilinearFraction );
				sampleNormal = mix( sampleNormal, sampleNormal2, trilinearFraction );
			}
		};
	};
	{
		sampleYCoCg.z = ( sampleYCoCg.z * 31.875 ) + 1.0;
		sampleYCoCg.z = 1.0 / sampleYCoCg.z;
		sampleYCoCg.xy *= sampleYCoCg.z;
		color.x = dot4( sampleYCoCg, matrixCoCg1YtoRGB1X );
		color.y = dot4( sampleYCoCg, matrixCoCg1YtoRGB1Y );
		color.z = dot4( sampleYCoCg, matrixCoCg1YtoRGB1Z );
		color.w = 0.5 - _fa_[4 ].x;
		color.xyz *= _fa_[5 ].xyz;
	};
	{
		specular.xyz = sampleSpecular.xyz * 8.0 / ( sampleNormal.z * 255.0 + 8.0 );
		specular.w = sampleNormal.x;
	};
	{
		float maxVal = ( max( max( specular.x, specular.y ), specular.z ) * 255.0 );
		float normalScale = 64.0 / max( min( maxVal, 64.0 ), 1.0 );
		float normalBlend = saturate( maxVal * 0.25 );
		vec3 globalSurfaceNormal = cross( dFdx( vofi_TexCoord6.xyz ), dFdy( vofi_TexCoord6.xyz ) );
		vec3 localNormal;
		localNormal.xy = ( sampleNormal.wy - vec2( 0.5 ) ) * vec2( 2.0 * normalScale );
		localNormal.z = sqrt( 1.0 - min( dot( localNormal.xy, localNormal.xy ), 1.0 ) );
		localNormal = normalize( localNormal );
		globalNormal.x = dot3( localNormal, vofi_TexCoord1.xyz );
		globalNormal.y = dot3( localNormal, vofi_TexCoord2.xyz );
		globalNormal.z = dot3( localNormal, vofi_TexCoord3.xyz );
		globalNormal = mix( normalize( globalSurfaceNormal ), globalNormal, normalBlend );
		globalNormal = normalize( globalNormal );
	};
	{
		specular.xyz *= _fa_[6 ].xyz * _fa_[7 ].xyz;
	};
	{
	};
	{
		vec4 reflection;
		{
			vec3 fromViewerN = normalize( vofi_TexCoord4.xyz );
			{
				reflection.xyz = fromViewerN - ( globalNormal * dot3( fromViewerN, globalNormal ) * 2.0 );
			};
			reflection.w = floor( ( 1.0 - specular.w ) * 4.0 + _fa_[8 ].y );
		};
		vec3 specularLight = vec3( 0.0 );
		vec3 envColor;
		{
			envColor = texCUBElod( samp_dynamicenvmap, reflection ).xyz;
		};
		vec2 powerAndScale = _fa_[9 ].xy * ( 1.0 - specular.w ) + _fa_[9 ].zw * specular.w;
		powerAndScale *= vec2( 0.1, 1.0 );
		vec3 fragmentToViewRay = normalize( -vofi_TexCoord4.xyz.xyz );
		vec3 lightToFragmentRay = normalize( vofi_TexCoord4.xyz + _fa_[10 ].xyz - _fa_[11 ].xyz );
		vec3 lightReflectRay = lightToFragmentRay - ( globalNormal * dot3( lightToFragmentRay, globalNormal ) * 2.0 );
		vec3 blendedReflectRay = normalize( globalNormal * ( 1.0 - _fa_[12 ].x ) + lightReflectRay * _fa_[12 ].x );
		float specularBrightness;
		{
			specularBrightness = pow( max( dot3( fragmentToViewRay, blendedReflectRay ), 0.0 ), max( powerAndScale.y, 0.000001 ) ) * powerAndScale.x;
		};
		specularBrightness *= max( 0.0, 1.0 - dot3( vofi_TexCoord4.xyz, vofi_TexCoord4.xyz ) / ( _fa_[13 ].x * _fa_[13 ].x ) );
		specularLight += _fa_[14 ].xyz * specularBrightness * specular.xyz;;
		specularLight = pow3( specularLight, 1.0 / 2.2 );
		specularLight += _fa_[15 ].xyz * envColor * specular.xyz;
		color.xyz += specularLight;
	};
	// RENODX: the scene colour target is upgraded from RGBA8 UNORM to RGBA16F. UNORM silently turned NaN into 0 and
	// clamped to [0, 1]; on degenerate geometry (e.g. wall-damage patches) this shader outputs NaN / Inf / huge values that
	// survived the upgrade (bright blocks after sharpen + bloom). Reproduce the UNORM conversion for this shader's colour.
	out_FragColor0 = clamp( mix( color, vec4( 0.0 ), isnan( color ) ), 0.0, 1.0 );
	out_FragColor1 = feedback;
	{
		out_FragColor2.xyz = globalNormal.xyz * vec3( 0.5 ) + vec3( 0.5 );
		out_FragColor2.w = dot( specular.rgb, vec3( 1.0 / 3.0 ) );
	};
}