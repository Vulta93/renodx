#version 150

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

float dot3 ( vec3 a, vec3 b ) { return dot( a, b ); }
float dot3 ( vec3 a, vec4 b ) { return dot( a, b.xyz ); }
float dot3 ( vec4 a, vec3 b ) { return dot( a.xyz, b ); }
float dot3 ( vec4 a, vec4 b ) { return dot( a.xyz, b.xyz ); }
vec4 h4tex2D ( sampler2D image, vec2 texcoord ) { return tex2D( image, texcoord ); }
float tex2Ddepth_fast ( sampler2D image, vec2 texcoord ) { return tex2D( image, texcoord ).x; }
vec2 screenPosToTexcoord ( vec2 pos, vec4 bias_scale ) { return ( pos * bias_scale.zw + bias_scale.xy ); }
uniform vec4 _fa_ [25];
uniform sampler2D samp_glaremap;
uniform sampler2D samp_viewdepthmap;
uniform sampler2D samp_postdistortionmap;
uniform sampler2D samp_viewcolormap;
uniform sampler2D samp_cbconversionlut;
uniform sampler2D samp_screenoverlay;
uniform sampler2D samp_grainmap;
uniform sampler2D samp_dynamiccc;

// RENODX: settings. Mirrors ShaderInjectData in shared.h field for field (32 floats). RenoDX pushes it through
// ReShade's OpenGL push-constant path, which uploads a UBO at binding 0 (the layout param's binding); an unbound uniform
// block also defaults to binding 0. std140 packs float members 4 bytes apart, matching the C++ struct.
struct ShaderInjectData {
	float peak_white_nits;
	float diffuse_white_nits;
	float graphics_white_nits;
	float color_grade_strength;
	float tone_map_type;
	float tone_map_exposure;
	float tone_map_highlights;
	float tone_map_shadows;
	float tone_map_contrast;
	float tone_map_saturation;
	float tone_map_highlight_saturation;
	float tone_map_blowout;
	float tone_map_flare;
	float tone_map_hue_correction;
	float tone_map_hue_shift;
	float tone_map_working_color_space;
	float tone_map_clamp_color_space;
	float tone_map_clamp_peak;
	float tone_map_hue_processor;
	float tone_map_per_channel;
	float gamma_correction;
	float intermediate_scaling;
	float intermediate_encoding;
	float intermediate_color_space;
	float swap_chain_decoding;
	float swap_chain_gamma_correction;
	float swap_chain_custom_color_space;
	float swap_chain_clamp_color_space;
	float swap_chain_encoding;
	float swap_chain_encoding_color_space;
	float custom_flip_uv_y;
	float padding0;
};
layout( std140 ) uniform RenoDXShaderInjection {
	ShaderInjectData shader_injection;
};

#define RENODX_TONE_MAP_TYPE         shader_injection.tone_map_type
#define RENODX_DIFFUSE_WHITE_NITS    shader_injection.diffuse_white_nits
#define RENODX_GRAPHICS_WHITE_NITS   shader_injection.graphics_white_nits
#define RENODX_INTERMEDIATE_ENCODING shader_injection.intermediate_encoding

// renodx::draw::DecodeColor / EncodeColor for the encodings the intermediate can use (draw.hlsl; sign-preserving
// DecodeSafe / EncodeSafe). 0 none, 1 sRGB, 2 gamma 2.2, 3 gamma 2.4.
vec3 renodx_srgb_decode( vec3 c ) { return mix( c / 12.92, pow( ( c + 0.055 ) / 1.055, vec3( 2.4 ) ), step( vec3( 0.04045 ), c ) ); }
vec3 renodx_srgb_encode( vec3 c ) { return mix( c * 12.92, 1.055 * pow( c, vec3( 1.0 / 2.4 ) ) - 0.055, step( vec3( 0.0031308 ), c ) ); }
vec3 renodx_decode( vec3 c, float encoding ) {
	if ( encoding == 1.0 ) return sign( c ) * renodx_srgb_decode( abs( c ) );
	if ( encoding == 2.0 ) return sign( c ) * pow( abs( c ), vec3( 2.2 ) );
	if ( encoding == 3.0 ) return sign( c ) * pow( abs( c ), vec3( 2.4 ) );
	return c;
}
vec3 renodx_encode( vec3 c, float encoding ) {
	if ( encoding == 1.0 ) return sign( c ) * renodx_srgb_encode( abs( c ) );
	if ( encoding == 2.0 ) return sign( c ) * pow( abs( c ), vec3( 1.0 / 2.2 ) );
	if ( encoding == 3.0 ) return sign( c ) * pow( abs( c ), vec3( 1.0 / 2.4 ) );
	return c;
}

in vec4 gl_FragCoord;

out vec4 out_FragColor0;

void main() {
	// RENODX: max-channel factor of the HDR scene (>= 1), see bridge in / out below.
	float renodx_scene_max = 1.0;
	vec2 viewTexCoord = screenPosToTexcoord( gl_FragCoord.xy, _fa_[0 ] );
	vec2 windowTexCoord = screenPosToTexcoord( gl_FragCoord.xy, _fa_[1 ] );
	windowTexCoord.y = 1.0 - windowTexCoord.y;
	vec2 offset;
	{
		vec2 scale = 2.0 * viewTexCoord.xy - 1.0;
		vec2 blah = saturate( vec2( 0.8 - scale.x * scale.x, 0.8 - scale.y * scale.y ) );
		offset = _fa_[2 ].xy * blah;
	};
	viewTexCoord += offset;
	vec4 glareTex = h4tex2D( samp_glaremap, viewTexCoord );
	float clipZ;
	{
		{
			clipZ = tex2Ddepth_fast( samp_viewdepthmap, viewTexCoord.xy ) * 2.0 - 1.0;
		};
		{
			clipZ = clipZ * _fa_[3 ].w / ( clipZ + _fa_[3 ].z ) - _fa_[3 ].w;
		};
	};
	float dof;
	{
		float a = saturate( ( _fa_[4 ].x - clipZ) * _fa_[5 ].x ) * _fa_[6 ].x;
		float b = saturate( (clipZ - _fa_[7 ].x) * _fa_[5 ].y ) * _fa_[8 ].x;
		dof = max(a, b);
		dof = (clipZ > 0.0) ? dof : _fa_[9 ].x;
		dof = dof* _fa_[10 ].x + _fa_[10 ].y;
	};
	float blur = 0.0;
	{
		vec2 perturb = vec2( 0.0 );
		vec4 distortion = h4tex2D( samp_postdistortionmap, viewTexCoord.xy ) - 0.5;
		perturb += distortion.xy * 0.1;
		blur += saturate( distortion.w ) * 16.0;
		viewTexCoord.xy += perturb.xy;
	};
	float blurMip = ( dof * _fa_[11 ].x ) + blur;
	blurMip = log2( blurMip * 5.0 + 1.0 );
	vec4 final;
	{
		if ( blurMip > 0.5 ) {
			float offset = 0.2;
			float sampleDof = blurMip - 1.5;
			vec2 xyStep = _fa_[0 ].zw * pow( 2, sampleDof - offset );
			sampleDof += offset;
			final = tex2Dlod( samp_viewcolormap, vec4( viewTexCoord + vec2( xyStep.x, xyStep.y ), 0.0, sampleDof ) );
			final += tex2Dlod( samp_viewcolormap, vec4( viewTexCoord + vec2( -xyStep.x, xyStep.y ), 0.0, sampleDof ) );
			final += tex2Dlod( samp_viewcolormap, vec4( viewTexCoord + vec2( 0.0 , 0.0 ), 0.0, sampleDof ) );
			final += tex2Dlod( samp_viewcolormap, vec4( viewTexCoord + vec2( xyStep.x, -xyStep.y ), 0.0, sampleDof ) );
			final += tex2Dlod( samp_viewcolormap, vec4( viewTexCoord + vec2( -xyStep.x, -xyStep.y ), 0.0, sampleDof ) );
			final *= ( 1.0 / 5.0 );
		} else {
			final = tex2Dlod( samp_viewcolormap, vec4( viewTexCoord, 0.0, blurMip ) );
		}
	};
	{ if ( _fa_[12 ].x != 0.0 ) {
			int numsamples = 32;
			vec4 col = vec4( 0.0 );
			for ( int i = 0; i < numsamples; ++i ) {
				float scale = 1.0 - ( _fa_[13 ].x ) * ( i / float( numsamples - 1.0 ) );
				col += tex2D( samp_viewcolormap, scale * viewTexCoord.xy + ( 1.0 - scale ) * _fa_[14 ].xy ) * ( numsamples - i ) * ( numsamples - i );
			}
			col = col / ( numsamples * numsamples * numsamples / 4 );
			float luminance = saturate( dot3( col.xyz, vec3( 0.33, 0.59, 0.11 ) ) ) * _fa_[15 ].x;
			final = mix( final, col, sqrt( luminance ) );
		}
	};
	{
		float sharpenAmount = 0.35;
		float maskIntensity = 7.0;
		sharpenAmount = max( 0.0, sharpenAmount - blurMip);
		if( sharpenAmount > 0.0 ) {
			vec3 blurred = tex2Dlod( samp_viewcolormap, vec4( viewTexCoord.xy, 0.0, blurMip + 1.0 ) ).xyz;
			float finalLuminance;
			float blurredLuminance;
			{
				vec3 ituBT601 = vec3( 0.299, 0.587, 0.114 );
				finalLuminance = dot( final.xyz * final.xyz, ituBT601 );
			};
			{
				vec3 ituBT601 = vec3( 0.299, 0.587, 0.114 );
				blurredLuminance = dot( blurred.xyz * blurred.xyz, ituBT601 );
			};
			{
				vec3 unsharpMask = vec3( saturate( maskIntensity * (finalLuminance - blurredLuminance) ) );
				vec3 sharpen = 5 * final.xyz -
				tex2Dlod( samp_viewcolormap, vec4( viewTexCoord.xy + vec2( _fa_[0 ].z, 0.0 ), 0.0, blurMip ) ).xyz -
				tex2Dlod( samp_viewcolormap, vec4( viewTexCoord.xy - vec2( _fa_[0 ].z, 0.0 ), 0.0, blurMip ) ).xyz -
				tex2Dlod( samp_viewcolormap, vec4( viewTexCoord.xy + vec2( 0.0, _fa_[0 ].w ), 0.0, blurMip ) ).xyz -
				tex2Dlod( samp_viewcolormap, vec4( viewTexCoord.xy - vec2( 0.0, _fa_[0 ].w ), 0.0, blurMip ) ).xyz;
				sharpen = mix( sharpen, final.xyz, unsharpMask );
				final.xyz = mix( final.xyz, sharpen, sharpenAmount );
			}
		}
	};
	{
		final = final * _fa_[16 ] + glareTex;
	};
	{ if ( any( notEqual( _fa_[2 ].xy, vec2( 0.0 ) ) ) ) {
			vec2 tc2 = screenPosToTexcoord( gl_FragCoord.xy, _fa_[0 ] );
			vec2 offset;
			{
				vec2 scale = 2.0 * viewTexCoord.xy - 1.0;
				vec2 blah = saturate( vec2( 0.8 - scale.x * scale.x, 0.8 - scale.y * scale.y ) );
				offset = _fa_[2 ].xy * blah;
			};
			tc2 -= offset;
			vec4 glareTex2 = tex2D( samp_glaremap, tc2.xy );
			vec4 final2 = tex2Dlod( samp_viewcolormap, vec4( tc2.xy, 0.0, blurMip ) ) * _fa_[16 ] + glareTex2;
			final = mix( final, final2, 0.5 );
		}
	};
	{
		vec3 tmpCol = vec3( dot3( final.xyz, vec3( 0.33, 0.59, 0.11 ) ) );
		final.xyz = mix( final.xyz, tmpCol, _fa_[17 ].x );
		// RENODX bridge in: identical to vanilla for every pixel <= 1.0. Above white the colour is divided by its
		// max channel (hue kept), so the game's curve, overlay, grain and LUT see the SDR range they expect.
		renodx_scene_max = max( 1.0, max( final.x, max( final.y, final.z ) ) );
		final.xyz /= renodx_scene_max;
		tmpCol.x = h4tex2D( samp_cbconversionlut, vec2( final.x, 0.0 ) ).x;
		tmpCol.y = h4tex2D( samp_cbconversionlut, vec2( final.y, 0.0 ) ).y;
		tmpCol.z = h4tex2D( samp_cbconversionlut, vec2( final.z, 0.0 ) ).z;
		tmpCol *= _fa_[18 ].x;
		final.xyz = mix( tmpCol.xyz, final.xyz, min( _fa_[19 ].x * dof, 1.0 ) );
	};
	{
		final.xyz *= _fa_[20 ].y + h4tex2D( samp_screenoverlay, windowTexCoord ).xyz * _fa_[20 ].x;
	};
	{
		vec2 graincoord = ( viewTexCoord.xy * _fa_[21 ].xy ) + _fa_[21 ].zw;
		vec3 grain = h4tex2D( samp_grainmap, graincoord ).xyz * _fa_[22 ].x + _fa_[22 ].y;
		float tempLum = dot( final.xyz, vec3( 0.3, 0.59, 0.11 ) );
		tempLum = saturate( ( tempLum * _fa_[22 ].z ) + _fa_[22 ].w );
		grain *= tempLum;
		final.xyz += grain;
		if ( _fa_[23 ].x == 1.0 ) {
			final.xyz = vec3( tempLum );
		}
	};
	{
		float ccTexDim = 16.0; float ccDim = 16.0;
		vec2 duvCC = vec2( 1.0 / ( ccTexDim * ccTexDim ), 1.0 / ccTexDim );
		vec3 tmp = saturate( final.xyz ) * ( ccDim - 1.0 );
		vec2 tcRG = tmp.xy * vec2( 1.0 / ( ccTexDim * ccTexDim ), ( 1.0 / ccTexDim ) ) + ( 0.5 * duvCC );
		float tcB = tmp.z;
		float tcBFrac = fract( tcB );
		float tcB1 = ceil( tcB ) * ( 1.0 / ccTexDim );
		float tcB0 = floor( tcB ) * ( 1.0 / ccTexDim );
		vec3 resCol0 = tex2Dlod( samp_dynamiccc, vec4( tcRG.x + tcB0, tcRG.y, 0, 0 ) ).xyz;
		vec3 resCol1 = tex2Dlod( samp_dynamiccc, vec4( tcRG.x + tcB1, tcRG.y, 0, 0 ) ).xyz;
		vec3 resCol = mix( resCol0, resCol1, tcBFrac );
		// RENODX bridge out: the graded SDR result scaled back up by the same factor. In this gamma-encoded space
		// (x^(1/2.2)) a scale is the same as scaling the linear colour, so this restores the HDR range on the grade.
		final.xyz = resCol * renodx_scene_max;
	};
	// RENODX: Game Brightness. The HUD is drawn on top of this output and the Display Proxy scales the whole frame by
	// UI Brightness (graphics white), so the scene alone is scaled by Game / UI in linear light here
	// (renodx::draw::RenderIntermediatePass convention). Not in Vanilla.
	if ( RENODX_TONE_MAP_TYPE != 0.0 ) {
		final.xyz = renodx_encode( renodx_decode( final.xyz, RENODX_INTERMEDIATE_ENCODING ) * ( RENODX_DIFFUSE_WHITE_NITS / RENODX_GRAPHICS_WHITE_NITS ), RENODX_INTERMEDIATE_ENCODING );
	}
	out_FragColor0.xyz = final.xyz;
	out_FragColor0.w = 1.0;
	if ( _fa_[24 ].x == 1.0 ) {
		out_FragColor0.xyz = vec3( dof );
	}
}