#version 450 core
layout(location = 0) out vec4 fColor;
layout(set=0, binding=0) uniform sampler2D sTexture;
layout(location = 0) in struct { vec4 Color; vec2 UV; } In;
#line 1 30
void main()
{
	fColor = vec4(vec3(255, 170, 170)/255.0, 1.0);//vec4(vec3(In.UV.st.x), 1.0);
	//fColor = vec4(In.Color.xyz * texture(sTexture, In.UV.st).xyz, 1.0);
	//fColor = vec4(In.Color.xyz * In.UV.st.x, 1.0);
    //fColor = In.Color * texture(sTexture, In.UV.st);
}