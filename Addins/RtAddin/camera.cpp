#include "camera.h"

using namespace DirectX;

constexpr uint32_t right = 0;
constexpr uint32_t up = 1;
constexpr uint32_t look = 2;

void FpsCamera::init(float _fov, float _aspect)
{
	fov = _fov;
	aspect = _aspect;
	proj = XMMatrixTranspose(XMMatrixPerspectiveFovLH(fov, aspect, 0.1f, 5000.0f));
	view = XMMatrixIdentity();
	pos = XMVectorZero();

	view.r[right] = XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
	view.r[up] = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
	view.r[look] = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
}

void FpsCamera::place(XMVECTOR _pos)
{
	pos = _pos;
}

void FpsCamera::look_at(XMVECTOR _pos, XMVECTOR target, XMVECTOR _up)
{
	pos = _pos;

	XMVECTOR L = XMVector3Normalize(target - pos);
	XMVECTOR R = XMVector3Cross(L, _up);
	XMVECTOR U = XMVector3Cross(R, L);

	view.r[right] = R;
	view.r[up] = U;
	view.r[look] = L;
}

void FpsCamera::rotate(float deltaX, float deltaY)
{
	//allow rotation on the up vector (z)
	if (fabs(deltaX) > 0.0f)
	{
		XMMATRIX R = XMMatrixRotationZ(deltaX);
		view.r[right] = XMVector3Transform(view.r[right], R);
		view.r[up] = XMVector3Transform(view.r[up], R);
		view.r[look] = XMVector3Transform(view.r[look], R);
	}		

	// now rotate on the side vector (x)
	if (fabs(deltaY) > 0.0f)
	{
		XMMATRIX R = XMMatrixRotationAxis(view.r[right], deltaY);
		view.r[up] = XMVector3Transform(view.r[up], R);
		view.r[look] = XMVector3Transform(view.r[look], R);
	}
}

void FpsCamera::move_forward(float step)
{
	pos += view.r[look] * step;
}

void FpsCamera::set_fov(float _fov)
{
	if (fov != _fov)
	{
		fov = _fov;
		proj = XMMatrixTranspose(XMMatrixPerspectiveFovLH(fov, aspect, 0.1f, 5000.0f));
	}
}

DirectX::XMVECTOR FpsCamera::get_pos()
{
	return pos;
}

DirectX::XMMATRIX FpsCamera::get_view_transform()
{
	normalize();
	return view;
}

DirectX::XMMATRIX FpsCamera::get_viewproj()
{
	normalize();

	auto dot = [](XMVECTOR &a, XMVECTOR &b) {
		return XMVectorGetX(XMVector3Dot(a, b));
	};

	float x = -dot(pos, view.r[right]);
	float y = -dot(pos, view.r[up]);
	float z = -dot(pos, view.r[look]);

	view.r[right] = XMVectorSetW(view.r[right], x);
	view.r[up] = XMVectorSetW(view.r[up], y);
	view.r[look] = XMVectorSetW(view.r[look], z);

	// we have row vectors so invert the mult order
	return proj * view;
}

void FpsCamera::normalize()
{
	view.r[look] = XMVector3Normalize(view.r[look]);

	view.r[up] = XMVector3Cross(view.r[right], view.r[look]);
	view.r[up] = XMVector3Normalize(view.r[up]);

	view.r[right] = XMVector3Cross(view.r[look], view.r[up]);
	view.r[right] = XMVector3Normalize(view.r[right]);
}

void GameCamera::set_view(DirectX::XMFLOAT3X4 &affine_view)
{
	view = XMMATRIX(
			XMLoadFloat4((XMFLOAT4 *)affine_view.m[0]),
			XMLoadFloat4((XMFLOAT4 *)affine_view.m[1]),
			XMLoadFloat4((XMFLOAT4 *)affine_view.m[2]),
			XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f));
	view = XMMatrixTranspose(view);
	view = XMMatrixInverse(0, view);
}

void GameCamera::set_viewproj(DirectX::XMMATRIX &_viewproj)
{
	viewproj = _viewproj;
}

DirectX::XMVECTOR GameCamera::get_pos()
{
	return view.r[3];
}

DirectX::XMMATRIX& GameCamera::get_viewproj()
{
	return viewproj;
}
