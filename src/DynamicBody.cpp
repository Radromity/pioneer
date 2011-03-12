#include "libs.h"
#include "DynamicBody.h"
#include "Space.h"
#include "Frame.h"
#include "Serializer.h"
#include "Planet.h"
#include "Pi.h"

DynamicBody::DynamicBody(): ModelBody()
{
	m_flags = Body::FLAG_CAN_MOVE_FRAME;
	m_orient = matrix4x4d::Identity();
	m_oldOrient = m_orient;
	m_oldAngDisplacement = vector3d(0.0);
	m_force = vector3d(0.0);
	m_torque = vector3d(0.0);
	m_vel = vector3d(0.0);
	m_angVel = vector3d(0.0);
	m_mass = 1;
	m_angInertia = 1;
	m_massRadius = 1;
	m_enabled = true;
	m_atmosForce = vector3d(0.0);
	m_gravityForce = vector3d(0.0);
	m_externalForce = vector3d(0.0);		// do external forces calc instead?
}

void DynamicBody::SetForce(const vector3d f)
{
	m_force = f;
}

void DynamicBody::AddForce(const vector3d f)
{
	m_force += f;
}

void DynamicBody::AddTorque(const vector3d t)
{
	m_torque += t;
}

void DynamicBody::AddRelForce(const vector3d f)
{
	m_force += m_orient.ApplyRotationOnly(f);
}

void DynamicBody::AddRelTorque(const vector3d t)
{
	m_torque += m_orient.ApplyRotationOnly(t);
}

void DynamicBody::Save(Serializer::Writer &wr)
{
	ModelBody::Save(wr);
	for (int i=0; i<16; i++) wr.Double(m_orient[i]);
	wr.Vector3d(m_force);
	wr.Vector3d(m_torque);
	wr.Vector3d(m_vel);
	wr.Vector3d(m_angVel);
	wr.Double(m_mass);
	wr.Double(m_massRadius);
	wr.Double(m_angInertia);
	wr.Bool(m_enabled);
}

void DynamicBody::Load(Serializer::Reader &rd)
{
	ModelBody::Load(rd);
	for (int i=0; i<16; i++) m_orient[i] = rd.Double();
	m_oldOrient = m_orient;
	m_force = rd.Vector3d();
	m_torque = rd.Vector3d();
	m_vel = rd.Vector3d();
	m_angVel = rd.Vector3d();
	m_mass = rd.Double();
	m_massRadius = rd.Double();
	m_angInertia = rd.Double();
	m_enabled = rd.Bool();
}

void DynamicBody::PostLoadFixup()
{
	CalcExternalForce();
}

void DynamicBody::SetTorque(const vector3d t)
{
	m_torque = t;
}

void DynamicBody::SetMass(double mass)
{
	m_mass = mass;
	// This is solid sphere mass distribution, my friend
	m_angInertia = (2/5.0)*m_mass*m_massRadius*m_massRadius;
}

void DynamicBody::SetPosition(vector3d p)
{
	m_orient[12] = p.x;
	m_orient[13] = p.y;
	m_orient[14] = p.z;
	ModelBody::SetPosition(p);
}

vector3d DynamicBody::GetPosition() const
{
	return vector3d(m_orient[12], m_orient[13], m_orient[14]);
}

void DynamicBody::CalcExternalForce()
{
	// gravity
	if (!GetFrame()->GetBodyFor()->IsType(Object::SPACESTATION)) {	// they ought to have mass though...
		vector3d b1b2 = GetPosition();
		double m1m2 = GetMass() * GetFrame()->GetBodyFor()->GetMass();
		double invrsqr = 1.0 / b1b2.LengthSqr();
		double force = G*m1m2 * invrsqr;
		m_externalForce = -b1b2 * sqrt(invrsqr) * force;
	}
	else m_externalForce = vector3d(0.0);
	m_gravityForce = m_externalForce;

	// atmospheric drag
	const double speed = m_vel.Length();
	if ((speed > 0) && GetFrame()->GetBodyFor()->IsType(Object::PLANET))
	{
		Planet *planet = static_cast<Planet*>(GetFrame()->GetBodyFor());
		double dist = GetPosition().Length();
		double pressure, density;
		planet->GetAtmosphericState(dist, &pressure, &density);
		const double radius = GetBoundingRadius();
		const double AREA = radius;
		// ^^^ yes that is as stupid as it looks
		const double DRAG_COEFF = 0.1; // 'smooth sphere'
		vector3d fDrag = -0.5*density*speed*speed*AREA*DRAG_COEFF*m_vel.Normalized();

		// make this a bit less daft at high time accel
		// better way? cap force to some percentage of velocity given current timestep...
		m_atmosForce += 0.01 * (fDrag - m_atmosForce);
//		else if (Pi::GetTimeAccel() > 100) m_atmosForce += 0.1 * (fDrag - m_atmosForce);
//		else m_atmosForce = fDrag;

		m_externalForce += m_atmosForce;
	}

	// centrifugal and coriolis forces for rotating frames
	vector3d angRot = GetFrame()->GetAngVelocity();
	if (angRot.LengthSqr() > 0.0) {
		m_externalForce -= m_mass * angRot.Cross(angRot.Cross(GetPosition()));	// centrifugal
		m_externalForce -= 2 * m_mass * angRot.Cross(GetVelocity());			// coriolis
	}

}

void DynamicBody::TimeStepUpdate(const float timeStep)
{
	if (m_enabled) {
		m_force += m_externalForce;

		m_oldOrient = m_orient;
		m_vel += (double)timeStep * m_force * (1.0 / m_mass);
		m_angVel += (double)timeStep * m_torque * (1.0 / m_angInertia);
		// angvel is always relative to non-rotating frame, so need to counter frame angvel
		vector3d consideredAngVel = m_angVel - GetFrame()->GetAngVelocity();
		
		vector3d pos = GetPosition();
		// applying angular velocity :-/
		{
			double len = consideredAngVel.Length();
			if (len != 0) {
				vector3d rotAxis = consideredAngVel * (1.0 / len);
				matrix4x4d rotMatrix = matrix4x4d::RotateMatrix(len * timeStep,
						rotAxis.x, rotAxis.y, rotAxis.z);
				m_orient = rotMatrix * m_orient;
			}
		}
		m_oldAngDisplacement = consideredAngVel * timeStep;

		pos += m_vel * (double)timeStep;
		m_orient[12] = pos.x;
		m_orient[13] = pos.y;
		m_orient[14] = pos.z;
		TriMeshUpdateLastPos(m_orient);

//printf("vel = %.1f,%.1f,%.1f, force = %.1f,%.1f,%.1f, external = %.1f,%.1f,%.1f\n",
//	m_vel.x, m_vel.y, m_vel.z, m_force.x, m_force.y, m_force.z,
//	m_externalForce.x, m_externalForce.y, m_externalForce.z);

		m_force = vector3d(0.0);
		m_torque = vector3d(0.0);
		CalcExternalForce();			// regenerate for new pos/vel
	} else {
		m_oldOrient = m_orient;
		m_oldAngDisplacement = vector3d(0.0);
	}
}

// for timestep changes, to stop autopilot overshoot
void DynamicBody::ApplyAccel(const float timeStep)
{
//	vector3d newvel = m_vel + (double)timeStep * m_force * (1.0 / m_mass);
//	if (newvel.LengthSqr() < m_vel.LengthSqr()) m_vel = newvel;
//	vector3d newav = m_angVel + (double)timeStep * m_torque * (1.0 / m_angInertia);
//	if (newav.LengthSqr() < m_angVel.LengthSqr()) m_angVel = newav;
}

void DynamicBody::UpdateInterpolatedTransform(double alpha)
{
	// interpolating matrices like this is a sure sign of madness
	vector3d outPos = alpha*vector3d(m_orient[12], m_orient[13], m_orient[14]) +
			(1.0-alpha)*vector3d(m_oldOrient[12], m_oldOrient[13], m_oldOrient[14]);

	m_interpolatedTransform = m_oldOrient;
	{
		double len = m_oldAngDisplacement.Length() * (double)alpha;
		if (len != 0) {
			vector3d rotAxis = m_oldAngDisplacement.Normalized();
			matrix4x4d rotMatrix = matrix4x4d::RotateMatrix(len,
					rotAxis.x, rotAxis.y, rotAxis.z);
			m_interpolatedTransform = rotMatrix * m_interpolatedTransform;
		}
	}
	m_interpolatedTransform[12] = outPos.x;
	m_interpolatedTransform[13] = outPos.y;
	m_interpolatedTransform[14] = outPos.z;
}

void DynamicBody::UndoTimestep()
{
	m_orient = m_oldOrient;
	TriMeshUpdateLastPos(m_orient);
	TriMeshUpdateLastPos(m_orient);
}

void DynamicBody::Enable()
{
	ModelBody::Enable();
	m_enabled = true;
}

void DynamicBody::Disable()
{
	ModelBody::Disable();
	m_enabled = false;
}

void DynamicBody::SetRotMatrix(const matrix4x4d &r)
{
	vector3d pos = GetPosition();
	m_oldOrient = m_orient;
	m_orient = r;
	m_oldAngDisplacement = vector3d(0.0);
	SetPosition(pos);
}

void DynamicBody::GetRotMatrix(matrix4x4d &m) const
{
	m = m_orient;
	m[12] = 0;
	m[13] = 0;
	m[14] = 0;
}

void DynamicBody::SetMassDistributionFromModel()
{
	LmrCollMesh *m = GetLmrCollMesh();
	// XXX totally arbitrarily pick to distribute mass over a half
	// bounding sphere area
	m_massRadius = m->GetBoundingRadius()*0.5f;
	SetMass(m_mass);
}

vector3d DynamicBody::GetAngularMomentum() const
{
	return m_angInertia * m_angVel;
}

DynamicBody::~DynamicBody()
{
}

vector3d DynamicBody::GetAngVelocity() const
{
	return m_angVel;
}

vector3d DynamicBody::GetVelocity() const
{
	return m_vel;
}

void DynamicBody::SetVelocity(vector3d v)
{
	m_vel = v;
}

void DynamicBody::SetAngVelocity(vector3d v)
{
	m_angVel = v;
}

#define KINETIC_ENERGY_MULT	0.00001f
bool DynamicBody::OnCollision(Object *o, Uint32 flags, double relVel)
{
	double kineticEnergy = 0;
	if (o->IsType(Object::DYNAMICBODY)) {
		kineticEnergy = KINETIC_ENERGY_MULT * m_mass * relVel * relVel;
	} else {
		const double v = GetVelocity().Length();		// unused... copypaste bug?
		kineticEnergy = KINETIC_ENERGY_MULT * m_mass * relVel * relVel;
	}
	if (kineticEnergy) OnDamage(o, (float)kineticEnergy);
	return true;
}
