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

in vec4 gl_FragCoord;

out vec4 out_FragColor0;

void main() {
	vec3 pre_curve = vec3( 0.0 );
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
		// RENODX MEASURE: raw signal before the 1D curve (which clamps at 1.0 through texture addressing).
		pre_curve = final.xyz;
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
		// RENODX PROOF: keep what the scene holds above white (the game clamps it away here).
		vec3 above_white = max( final.xyz - 1.0, vec3( 0.0 ) );
		vec3 tmp = saturate( final.xyz ) * ( ccDim - 1.0 );
		vec2 tcRG = tmp.xy * vec2( 1.0 / ( ccTexDim * ccTexDim ), ( 1.0 / ccTexDim ) ) + ( 0.5 * duvCC );
		float tcB = tmp.z;
		float tcBFrac = fract( tcB );
		float tcB1 = ceil( tcB ) * ( 1.0 / ccTexDim );
		float tcB0 = floor( tcB ) * ( 1.0 / ccTexDim );
		vec3 resCol0 = tex2Dlod( samp_dynamiccc, vec4( tcRG.x + tcB0, tcRG.y, 0, 0 ) ).xyz;
		vec3 resCol1 = tex2Dlod( samp_dynamiccc, vec4( tcRG.x + tcB1, tcRG.y, 0, 0 ) ).xyz;
		vec3 resCol = mix( resCol0, resCol1, tcBFrac );
		final.xyz = resCol + above_white;
	};
	// RENODX MEASURE: output the pre-curve signal; tiny graded term keeps every uniform in use.
	final.xyz = pre_curve + final.xyz * 1e-6;
	out_FragColor0.xyz = final.xyz;
	out_FragColor0.w = 1.0;
	if ( _fa_[24 ].x == 1.0 ) {
		out_FragColor0.xyz = vec3( dof );
	}
}