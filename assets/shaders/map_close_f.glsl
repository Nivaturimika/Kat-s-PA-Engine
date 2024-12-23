in vec2 tex_coord;
out vec4 frag_color;

uniform sampler2D provinces_texture_sampler;
uniform sampler2D terrain_texture_sampler;
uniform sampler2DArray terrainsheet_texture_sampler;
uniform sampler2D water_normal;
uniform sampler2D colormap_water;
uniform sampler2D colormap_terrain;
uniform sampler2D overlay;
uniform sampler2DArray province_color;
uniform sampler2D colormap_political;
uniform sampler2D province_highlight;
uniform sampler2D stripes_texture;
uniform sampler2D province_fow;
uniform usampler2D diag_border_identifier;
uniform uint subroutines_index_2;
// location 0 : offset
// location 1 : zoom
// location 2 : screen_size
uniform vec2 map_size;
uniform float time;
vec4 gamma_correct(in vec4 colour);

// sheet is composed of 64 files, in 4 cubes of 4 rows of 4 columns
// so each column has 8 tiles, and each row has 8 tiles too
float xx = 1.f / map_size.x;
float yy = 1.f / map_size.y;
vec2 pix = vec2(xx, yy);

vec2 get_corrected_coords(vec2 coords) {
	coords.y -= (1 - 1 / 1.3) * 3 / 5;
	coords.y *= 1.3;
	return coords;
}

vec2 get_rounded_tex_coords(vec2 tex_coords) {
	vec2 rounded_tex_coords = (floor(tex_coord * map_size) + vec2(0.5, 0.5)) / map_size;
	vec2 rel_coord = tex_coord * map_size - floor(tex_coord * map_size) - vec2(0.5);
	uint test = texture(diag_border_identifier, rounded_tex_coords).x;
	int shift = int(sign(rel_coord.x) + 2 * sign(rel_coord.y) + 3);
	rounded_tex_coords.y += ((int(test >> shift) & 1) != 0) && (abs(rel_coord.x) + abs(rel_coord.y) > 0.5) ? sign(rel_coord.y) / map_size.y : 0;
	return rounded_tex_coords;
}

// The water effect
vec4 get_water_terrain() {
	// Water effect taken from Vic2 fx/water/PixelShader_HoiWater_2_0
	const float WRAP = 0.8f;
	const float WaveModOne = 3.f;
	const float WaveModTwo = 4.f;
	const float SpecValueOne = 8.f;
	const float SpecValueTwo = 2.f;
	const float vWaterTransparens = 1.f; //more transparance lets you see more of background
	const float vColorMapFactor = 1.f; //how much colormap

	vec2 tex_coord = tex_coord;
	vec2 corrected_coord = get_corrected_coords(tex_coord);
	vec2 rounded_tex_coords = get_rounded_tex_coords(tex_coord);
	vec2 prov_id = texture(provinces_texture_sampler, rounded_tex_coords).xy;
	vec3 WorldColorColor = texture(colormap_water, corrected_coord).rgb;
	tex_coord = tex_coord * 25.f + time * 0.002f;

	vec2 coordA = tex_coord * 3.f + vec2(0.1f, 0.1f);

	// Uses textureNoTile for non repeting textures,
	// probably unnecessarily expensive
	vec4 vBumpA = texture(water_normal, coordA);
	vec2 coordB = tex_coord * 2.8f + vec2(0.0f, 0.1f);
	coordB += vec2(0.03f * sin(1.11f * time), -0.02f * cos(0.25f * time));
	vec4 vBumpB = texture(water_normal, coordB);
	vec2 coordC = tex_coord * 2.3f + vec2(0.0f, 0.15f);
	coordC += vec2(0.03f * sin(0.88f * time), -0.01f * cos(0.51f * time));
	vec4 vBumpC = texture(water_normal, coordC);
	vec2 coordD = tex_coord * 5.5f + vec2(0.0f, 0.3f);
	coordD += vec2(0.02f * sin(0.98f * time), -0.01f * cos(0.57f * time));
	vec4 vBumpD = texture(water_normal, coordD);

	vec3 vBumpTex = normalize(WaveModOne * (vBumpA.xyz + vBumpB.xyz + vBumpC.xyz + vBumpD.xyz) - WaveModTwo);

	const vec3 lightDirection = vec3(0.f, 1.f, 1.f);
	const vec3 eyeDir = normalize(vec3(0.f, 1.f, 1.f));
	float NdotL = max(dot(eyeDir, (vBumpTex / 2.f)), 0.f);

	NdotL = clamp((NdotL + WRAP) / (1 + WRAP), 0.f, 1.f);
	NdotL = mix(NdotL, 1.0, 0.0);

	vec3 OutColor = NdotL * (WorldColorColor * vColorMapFactor);

	vec3	reflVector = -reflect(lightDirection, vBumpTex);
	float	specular = dot(normalize(reflVector), eyeDir);
	specular = clamp(specular, 0.f, 1.f);

	specular = pow(specular, SpecValueOne);
	OutColor += (specular / SpecValueTwo);
	OutColor *= COLOR_LIGHTNESS;
	OutColor *= texture(province_fow, prov_id).r;
	return vec4(OutColor, vWaterTransparens);
}

float get_terrain_index(vec2 corner) {
	float index = texture(terrain_texture_sampler, floor(tex_coord * map_size + vec2(0.5f)) / map_size + 0.5f * pix * corner).r;
	return floor(index * 256.f);
}

// The terrain color from the current texture coordinate offset with one pixel in the "corner" direction
vec4 get_terrain(vec2 corner, vec2 offset) {
	float index = get_terrain_index(corner);
	float is_water = step(64.f, index);
	vec4 colour = texture(terrainsheet_texture_sampler, vec3(offset, index));
	return vec4(colour.rgb, 1.f - is_water);
}

vec4 get_terrain_mix() {
	vec3 noise = vec3(145.f / 255.f, 163.f / 255.f, 189.f / 255.f);

	// Pixel size on map texture
	vec2 scaling = fract(tex_coord * map_size + vec2(0.5f));

	vec2 offset = tex_coord / (8.f * pix);

	vec4 colourlu = get_terrain(vec2(-1.f, -1.f), offset);
	vec4 colourld = get_terrain(vec2(-1.f, 1.f), offset);
	vec4 colourru = get_terrain(vec2(1.f, -1.f), offset);
	vec4 colourrd = get_terrain(vec2(1.f, 1.f), offset);

	// Mix together the terrains based on close they are to the current texture coordinate
	vec4 colour_u = mix(colourlu, colourru, scaling.x);
	vec4 colour_d = mix(colourld, colourrd, scaling.x);
	vec4 terrain = mix(colour_u, colour_d, scaling.y);

	// Mixes the terrains from "texturesheet.tga" with the "colormap.dds" background color.
	vec4 terrain_background = texture(colormap_terrain, get_corrected_coords(tex_coord));
	terrain.rgb = (terrain.rgb * 2.f + terrain_background.rgb) / 3.f;
	return terrain;
}

vec4 get_land_terrain() {
	return get_terrain_mix();
}

vec4 get_land_political_close() {
	vec4 terrain = get_terrain_mix();
	float is_land = terrain.a;

	// Make the terrain a gray scale color
	const vec3 GREYIFY = vec3(0.212671f, 0.715160f, 0.072169f);
	float grey = dot(terrain.rgb, GREYIFY);
	terrain.rgb = vec3(grey);

	vec2 tex_coords = tex_coord;
	vec2 rounded_tex_coords = get_rounded_tex_coords(tex_coords);
	vec2 prov_id = texture(provinces_texture_sampler, rounded_tex_coords).xy;

	// The primary and secondary map mode province colors
	vec4 prov_color = texture(province_color, vec3(prov_id, 0.f));
	vec4 stripe_color = texture(province_color, vec3(prov_id, 1.f));

	vec2 stripe_coord = tex_coord * vec2(512., 512. * map_size.y / map_size.x);

	// Mix together the primary and secondary colors with the stripes
	float stripeFactor = texture(stripes_texture, stripe_coord).a;
	float prov_highlight = texture(province_highlight, prov_id).r * (abs(cos(time * 3.f)) + 1.f);
	vec3 political = clamp(mix(prov_color, stripe_color, stripeFactor) + vec4(prov_highlight), 0.0, 1.0).rgb;
	political *= texture(province_fow, prov_id).r;
	political -= POLITICAL_LIGHTNESS;

	// Mix together the terrain and map mode color
	terrain.rgb = mix(terrain.rgb, political, POLITICAL_TERRAIN_MIX);
	terrain.rgb *= COLOR_LIGHTNESS;
	return terrain;
}

vec4 get_land() {
	switch(int(subroutines_index_2)) {
case 0: return get_land_terrain();
case 1: return get_land_political_close();
default: break;
	}
	return vec4(1.f);
}
vec4 get_water() {
	switch(int(subroutines_index_2)) {
case 0: return get_water_terrain();
case 1: return get_water_terrain();
default: break;
	}
	return vec4(1.f);
}

// The terrain map
// No province color is used here
void main() {
	vec4 terrain = get_land();
	vec4 water = get_water();
	frag_color.rgb = mix(water.rgb, terrain.rgb, min(1.f, floor(0.5f + terrain.a)));
	frag_color.a = 1.f;
	frag_color = gamma_correct(frag_color);
}
