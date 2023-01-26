#pragma once

#include <DirectXMath.h>

struct FpsCamera
{
	void init(float fov, float aspect);
	void look_at(const DirectX::XMVECTOR &pos, const DirectX::XMVECTOR &target);
	void rotate(float deltaX, float deltaY);
	void move_forward(float step);

	DirectX::XMVECTOR getPos();
	DirectX::XMMATRIX getView();
	DirectX::XMMATRIX getInvViewProj();
	void normalize();

	DirectX::XMMATRIX view;
	DirectX::XMMATRIX proj;
	DirectX::XMVECTOR pos;
};
