#pragma once

#include <DirectXMath.h>

struct FpsCamera
{
	void init(float fov, float aspect);
	void place(DirectX::XMVECTOR pos);
	void look_at(DirectX::XMVECTOR pos, DirectX::XMVECTOR target, DirectX::XMVECTOR up);
	void rotate(float deltaX, float deltaY);
	void move_forward(float step);
	void move_lateral(float step);
	void set_fov(float fov);

	DirectX::XMVECTOR get_pos();
	DirectX::XMMATRIX get_view_transform();
	DirectX::XMMATRIX get_viewproj();
	void normalize();

private:
	DirectX::XMMATRIX view;
	DirectX::XMMATRIX proj;
	DirectX::XMVECTOR pos;

	float fov;
	float aspect;
};

struct GameCamera
{
	void set_view(DirectX::XMFLOAT3X4 &affine_view);
	void set_viewproj(DirectX::XMMATRIX &viewproj);

	DirectX::XMVECTOR get_pos();
	DirectX::XMMATRIX& get_viewproj();

private:
	DirectX::XMVECTOR pos;
	DirectX::XMMATRIX view;
	DirectX::XMMATRIX viewproj;
};
