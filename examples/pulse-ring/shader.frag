// PocketDAW Visual: Pulse Ring
// Bass-reactive expanding rings with beat-synced flash

precision mediump float;

uniform float u_time;
uniform vec2  u_resolution;
uniform float u_bass;
uniform float u_mid;
uniform float u_high;
uniform float u_beat;
uniform float u_volume;
uniform float u_param0;  // Ring count (mapped 1-8)
uniform float u_param1;  // Color speed

void main() {
    vec2 uv = gl_FragCoord.xy / u_resolution;
    vec2 center = uv - 0.5;
    float dist = length(center);
    float angle = atan(center.y, center.x);
    
    // Number of rings
    float ringCount = 1.0 + u_param0 * 7.0;
    
    // Expanding rings that pulse with bass
    float ringPhase = fract(dist * ringCount - u_time * 0.5);
    float ring = smoothstep(0.0, 0.05, ringPhase) 
               - smoothstep(0.05, 0.1 + u_bass * 0.15, ringPhase);
    
    // Color based on angle and time
    float speed = 0.5 + u_param1 * 4.5;
    vec3 color = vec3(
        0.5 + 0.5 * sin(angle * 3.0 + u_time * speed),
        0.5 + 0.5 * sin(angle * 3.0 + u_time * speed + 2.094),
        0.5 + 0.5 * sin(angle * 3.0 + u_time * speed + 4.189)
    );
    
    // Mid frequencies add shimmer
    color += u_mid * 0.3 * vec3(sin(u_time * 10.0 + dist * 20.0));
    
    // Beat flash — bright pulse on each beat
    float flash = pow(max(0.0, 1.0 - u_beat * 4.0), 2.0);
    
    // Vignette
    float vignette = 1.0 - dist * 1.2;
    
    vec3 final_color = color * ring * vignette + flash * 0.15;
    final_color *= 0.5 + u_volume * 0.5; // Dim when quiet
    
    gl_FragColor = vec4(final_color, 1.0);
}
