#include "camera.h"

using namespace DirectX;

constexpr uint32_t right = 0;
constexpr uint32_t up = 1;
constexpr uint32_t look = 2;

void FpsCamera::init(float fov, float aspect)
{
	proj = XMMatrixPerspectiveFovRH(fov, aspect, 0.1f, 5000.0f);
	view = XMMatrixIdentity();
	pos = XMVectorZero();

	view.r[right] = XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
	view.r[up] = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
	view.r[look] = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
}

void FpsCamera::look_at(const DirectX::XMVECTOR &pos, const DirectX::XMVECTOR &target)
{
	view = XMMatrixInverse(nullptr, XMMatrixLookAtLH(pos, target, XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f)));
}

void FpsCamera::rotate(float deltaX, float deltaY)
{
	//allow rotation on the up vector (z)
	if (fabs(deltaX) > 0.0f)
	{
		view = XMMatrixRotationAxis(XMVectorSet(0.0f, 0.0f, -1.0f, 0.0f), deltaX) * view;
		//view = view * XMMatrixRotationAxis(XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), deltaX);
	}		

	// now rotate on the side vector (y)
	if (fabs(deltaY) > 0.0f)
	{
		XMVECTOR &rightv = view.r[right];
		view = XMMatrixRotationAxis(rightv, deltaY) * view;
		//view = view * XMMatrixRotationAxis(right, deltaY);
	}
}

void FpsCamera::move_forward(float step)
{
	XMMATRIX t = XMMatrixTranspose(view);
	XMVECTOR &forward = t.r[look];
	pos += forward * step;
}

DirectX::XMVECTOR FpsCamera::getPos()
{
	return pos;
}

DirectX::XMMATRIX FpsCamera::getView()
{
	return view;
}

DirectX::XMMATRIX FpsCamera::getInvViewProj()
{
	XMMATRIX viewtrans = XMMatrixTranslationFromVector(pos);
	XMMATRIX viewproj = view * viewtrans * proj;
	return XMMatrixInverse(nullptr, viewproj);
}

void FpsCamera::normalize()
{
	XMVECTOR r = view.r[right];
	XMVECTOR u = view.r[up];
	XMVECTOR l = view.r[look];

	float x = XMVectorGetW(view.r[0]);
	float y = XMVectorGetW(view.r[1]);
	float z = XMVectorGetW(view.r[2]);

	r = XMVector3Normalize(r);
	u = XMVector3Normalize(u);
	l = XMVector3Normalize(l);

	view.r[right] = r;
	view.r[up] = u;
	view.r[look] = l;

	view.r[0] = XMVectorSetW(view.r[0], x);
	view.r[1] = XMVectorSetW(view.r[1], y);
	view.r[2] = XMVectorSetW(view.r[2], z);
}
