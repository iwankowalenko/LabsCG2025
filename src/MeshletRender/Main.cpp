#include "stdafx.h"
#include "D3D12MeshletRender.h"

_Use_decl_annotations_
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    D3D12MeshletRender sample(1280, 720, L"D3D12 Mesh Shader");
    return Win32Application::Run(&sample, hInstance, nCmdShow);
}
