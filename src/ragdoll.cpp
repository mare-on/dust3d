#define _USE_MATH_DEFINES
#include <cmath>
#include <LinearMath/btDefaultMotionState.h>
#include <LinearMath/btAlignedAllocator.h>
#include <BulletCollision/CollisionShapes/btCapsuleShape.h>
#include <BulletDynamics/ConstraintSolver/btHingeConstraint.h>
#include <BulletDynamics/ConstraintSolver/btConeTwistConstraint.h>
#include <BulletDynamics/ConstraintSolver/btTypedConstraint.h>
#include <QQuaternion>
#include <QtMath>
#include <QMatrix4x4>
#include "ragdoll.h"
#include "poser.h"

RagDoll::RagDoll(const std::vector<RiggerBone> *rigBones) :
    m_jointNodeTree(rigBones),
    m_setpJointNodeTree(rigBones)
{
    if (nullptr == rigBones)
        return;
    
    m_bones = *rigBones;
    
    for (const auto &bone: *rigBones) {
        float groundY = bone.headPosition.y() - bone.headRadius;
        if (groundY < m_groundY)
            m_groundY = groundY;
        groundY = bone.tailPosition.y() - bone.tailRadius;
        if (groundY < m_groundY)
            m_groundY = groundY;
    }
    qDebug() << "m_groundY:" << m_groundY;
    
    createDynamicsWorld();
    
    std::map<QString, int> boneNameToIndexMap;
    
    std::map<QString, std::vector<QString>> chains;
    std::vector<QString> boneNames;
    for (const auto &bone: *rigBones) {
        boneNameToIndexMap[bone.name] = bone.index;
        boneNames.push_back(bone.name);
    }
    Poser::fetchChains(boneNames, chains);
    
    // Setup the geometry
    for (const auto &bone: *rigBones) {
        float radius = (bone.headRadius + bone.tailRadius) * 0.5;
        float height = bone.headPosition.distanceToPoint(bone.tailPosition);
        m_boneLengthMap[bone.name] = height;
        m_boneShapes[bone.name] = new btCapsuleShape(btScalar(radius), btScalar(height));
        m_boneShapes[bone.name]->setUserIndex(bone.index);
        m_bulletCodes += QString("m_boneShapes[\"%1\"] = new btCapsuleShape(btScalar(%2), btScalar(%3));")
            .arg(bone.name)
            .arg(QString::number(radius))
            .arg(QString::number(height));
    }
    
    // Setup all the rigid bodies
    m_boneInitialTransforms.resize(rigBones->size());
    for (const auto &bone: *rigBones) {
        btCollisionShape *shape = m_boneShapes[bone.name];
        btTransform transform;
        QVector3D middlePosition = (bone.headPosition + bone.tailPosition) * 0.5;
        m_boneMiddleMap[bone.name] = middlePosition;
        transform.setIdentity();
        transform.setOrigin(btVector3(
            btScalar(middlePosition.x()),
            btScalar(middlePosition.y()),
            btScalar(middlePosition.z())
        ));
        QVector3D to = (bone.tailPosition - bone.headPosition).normalized();
        QVector3D from = QVector3D(0, 1, 0);
        QQuaternion rotation = QQuaternion::rotationTo(from, to);
        btQuaternion btRotation(btScalar(rotation.x()), btScalar(rotation.y()), btScalar(rotation.z()),
            btScalar(rotation.scalar()));
        transform.getBasis().setRotation(btRotation);
        m_boneInitialTransforms[bone.index] = transform;
        m_boneBodies[bone.name] = createRigidBody(btScalar(1.), transform, shape);
        m_bulletCodes += QString("{");
        m_bulletCodes += QString("    btTransform transform;");
        m_bulletCodes += QString("    transform.setIdentity();");
        m_bulletCodes += QString("    transform.setOrigin(btVector3(btScalar(%1), btScalar(%2), btScalar(%3)));")
            .arg(QString::number(middlePosition.x()))
            .arg(QString::number(middlePosition.y()))
            .arg(QString::number(middlePosition.z()));
        m_bulletCodes += QString("    m_boneBodies[\"%1\"] = createRigidBody(btScalar(1.), transform, shape);")
            .arg(bone.name);
        m_bulletCodes += QString("}");
    }
    
    // Setup some damping on the m_bodies
    //for (const auto &bone: *rigBones) {
    //    m_boneBodies[bone.name]->setDamping(btScalar(0.05), btScalar(0.85));
    //    m_boneBodies[bone.name]->setDeactivationTime(btScalar(0.8));
    //    m_boneBodies[bone.name]->setSleepingThresholds(btScalar(1.6), btScalar(2.5));
    //}
    
    for (const auto &it: chains) {
        auto axis = chainNameToHingeAxis(it.first);
        for (size_t i = 1; i < it.second.size(); ++i) {
            const auto &parent = (*rigBones)[boneNameToIndexMap[it.second[i - 1]]];
            const auto &child = (*rigBones)[boneNameToIndexMap[it.second[i]]];
            btRigidBody *parentBoneBody = m_boneBodies[parent.name];
            btRigidBody *childBoneBody = m_boneBodies[child.name];
            float parentLength = m_boneLengthMap[parent.name];
            float childLength = m_boneLengthMap[child.name];
            const btVector3 btPivotA(0, parentLength * 0.5, 0.0f);
            const btVector3 btPivotB(0, -childLength * 0.5, 0.0f);
            {
                btVector3 btAxisA = axis;
                btVector3 btAxisB = axis;
                btHingeConstraint *constraint = new btHingeConstraint(*parentBoneBody, *childBoneBody,
                    btPivotA, btPivotB,
                    btAxisA, btAxisB);
                constraint->setLimit(btScalar(0), btScalar(0));
                m_boneConstraints.push_back(constraint);
            }
            m_bulletCodes += QString("{");
            m_bulletCodes += QString("    const btVector3 btPivotA(0, %1 * 0.5, 0.0f);")
                .arg(QString::number(parentLength));
            m_bulletCodes += QString("    const btVector3 btPivotB(0, %1 * 0.5, 0.0f);")
                .arg(QString::number(-childLength));
            m_bulletCodes += QString("    btVector3 btAxisA = btVector3(btScalar(%1), btScalar(%2), btScalar(%3));")
                .arg(QString::number(axis.x()))
                .arg(QString::number(axis.y()))
                .arg(QString::number(axis.z()));
            m_bulletCodes += QString("    btVector3 btAxisB = btAxisA;");
            m_bulletCodes += QString("    btHingeConstraint *constraint = new btHingeConstraint(");
            m_bulletCodes += QString("        *m_boneBodies[\"%1\"], *m_boneBodies[\"%2\"],")
                .arg(parent.name)
                .arg(child.name);
            m_bulletCodes += QString("        btPivotA, btPivotB,");
            m_bulletCodes += QString("        btAxisA, btAxisB);");
            m_bulletCodes += QString("    constraint->setLimit(btScalar(0), btScalar(0));");
            m_bulletCodes += QString("    m_boneConstraints.push_back(constraint);");
            m_bulletCodes += QString("}");
        }
    }
    
    for (const auto &bone: *rigBones) {
        if (bone.name.startsWith("Virtual_")) {
            const auto &parent = bone;
            float parentLength = m_boneLengthMap[parent.name];
            for (const auto &childIndex: bone.children) {
                const auto &child = (*rigBones)[childIndex];
                btRigidBody *parentBoneBody = m_boneBodies[parent.name];
                btRigidBody *childBoneBody = m_boneBodies[child.name];
                auto axis = chainNameToHingeAxis(child.name);
                float childLength = m_boneLengthMap[child.name];
                const btVector3 btPivotA(0, parentLength * 0.5, 0.0f);
                const btVector3 btPivotB(0, -childLength * 0.5, 0.0f);
                btVector3 btAxisA = axis;
                btVector3 btAxisB = axis;
                btHingeConstraint *constraint = new btHingeConstraint(*parentBoneBody, *childBoneBody,
                    btPivotA, btPivotB,
                    btAxisA, btAxisB);
                constraint->setLimit(btScalar(0), btScalar(0));
                m_boneConstraints.push_back(constraint);
                
                m_bulletCodes += QString("{");
                m_bulletCodes += QString("    const btVector3 btPivotA(0, %1 * 0.5, 0.0f);")
                    .arg(QString::number(parentLength));
                m_bulletCodes += QString("    const btVector3 btPivotB(0, %1 * 0.5, 0.0f);")
                    .arg(QString::number(-childLength));
                m_bulletCodes += QString("    btVector3 btAxisA = btVector3(btScalar(%1), btScalar(%2), btScalar(%3));")
                    .arg(QString::number(axis.x()))
                    .arg(QString::number(axis.y()))
                    .arg(QString::number(axis.z()));
                m_bulletCodes += QString("    btVector3 btAxisB = btAxisA;");
                m_bulletCodes += QString("    btHingeConstraint *constraint = new btHingeConstraint(");
                m_bulletCodes += QString("        *m_boneBodies[\"%1\"], *m_boneBodies[\"%2\"],")
                    .arg(parent.name)
                    .arg(child.name);
                m_bulletCodes += QString("        btPivotA, btPivotB,");
                m_bulletCodes += QString("        btAxisA, btAxisB);");
                m_bulletCodes += QString("    constraint->setLimit(btScalar(0), btScalar(0));");
                m_bulletCodes += QString("    m_boneConstraints.push_back(constraint);");
                m_bulletCodes += QString("}");
            }
        }
    }
    
    for (const auto &bone: *rigBones) {
        const auto &parent = bone;
        btRigidBody *parentBoneBody = m_boneBodies[parent.name];
        auto axis = chainNameToHingeAxis(parent.name);
        float parentLength = m_boneLengthMap[parent.name];
        for (const auto &childIndex: bone.children) {
            const auto &child = (*rigBones)[childIndex];
            if (child.name.startsWith("Virtual_")) {
                btRigidBody *childBoneBody = m_boneBodies[child.name];
                float childLength = m_boneLengthMap[child.name];
                const btVector3 btPivotA(0, parentLength * 0.5, 0.0f);
                const btVector3 btPivotB(0, -childLength * 0.5, 0.0f);
                btVector3 btAxisA = axis;
                btVector3 btAxisB = axis;
                btHingeConstraint *constraint = new btHingeConstraint(*parentBoneBody, *childBoneBody,
                    btPivotA, btPivotB,
                    btAxisA, btAxisB);
                constraint->setLimit(btScalar(0), btScalar(0));
                m_boneConstraints.push_back(constraint);
                
                m_bulletCodes += QString("{");
                m_bulletCodes += QString("    const btVector3 btPivotA(0, %1 * 0.5, 0.0f);")
                    .arg(QString::number(parentLength));
                m_bulletCodes += QString("    const btVector3 btPivotB(0, %1 * 0.5, 0.0f);")
                    .arg(QString::number(-childLength));
                m_bulletCodes += QString("    btVector3 btAxisA = btVector3(btScalar(%1), btScalar(%2), btScalar(%3));")
                    .arg(QString::number(axis.x()))
                    .arg(QString::number(axis.y()))
                    .arg(QString::number(axis.z()));
                m_bulletCodes += QString("    btVector3 btAxisB = btAxisA;");
                m_bulletCodes += QString("    btHingeConstraint *constraint = new btHingeConstraint(");
                m_bulletCodes += QString("        *m_boneBodies[\"%1\"], *m_boneBodies[\"%2\"],")
                    .arg(parent.name)
                    .arg(child.name);
                m_bulletCodes += QString("        btPivotA, btPivotB,");
                m_bulletCodes += QString("        btAxisA, btAxisB);");
                m_bulletCodes += QString("    constraint->setLimit(btScalar(0), btScalar(0));");
                m_bulletCodes += QString("    m_boneConstraints.push_back(constraint);");
                m_bulletCodes += QString("}");
            }
        }
    }
    
    for (auto &constraint: m_boneConstraints) {
        m_world->addConstraint(constraint, true);
    }
    
    m_bulletCodes += QString("{");
    m_bulletCodes += QString("    for (auto &constraint: m_boneConstraints) {");
    m_bulletCodes += QString("        m_world->addConstraint(constraint, true);");
    m_bulletCodes += QString("    }");
    m_bulletCodes += QString("}");
    
    qDebug() << m_bulletCodes.join("\r\n");
}

bool RagDoll::isPhysicsBone(const QString &boneName)
{
    if (boneName.startsWith("Virtual_"))
        return false;
    return true;
}

btVector3 RagDoll::chainNameToHingeAxis(const QString &chainName)
{
    if (chainName.startsWith("Spine"))
        return btVector3(1.0f, 0.0f, 0.0f);
    return btVector3(0.0f, 0.0f, 1.0f);
}

RagDoll::~RagDoll()
{
    // Remove all bodies and shapes
    for (auto &constraint: m_boneConstraints) {
        delete constraint;
    }
    m_boneConstraints.clear();
    for (auto &body: m_boneBodies) {
        if (nullptr == body.second)
            continue;
        m_world->removeRigidBody(body.second);
        delete body.second->getMotionState();
        delete body.second;
    }
    m_boneBodies.clear();
    for (auto &shape: m_boneShapes) {
        delete shape.second;
    }
    m_boneShapes.clear();
    
    delete m_groundShape;
    delete m_groundBody;
    
    delete m_world;
    
    delete m_collisionConfiguration;
    delete m_collisionDispather;
    delete m_broadphase;
    delete m_constraintSolver;
}

void RagDoll::createDynamicsWorld()
{
    m_collisionConfiguration = new btDefaultCollisionConfiguration();
    m_collisionDispather = new btCollisionDispatcher(m_collisionConfiguration);
    m_broadphase = new btDbvtBroadphase();
    m_constraintSolver = new btSequentialImpulseConstraintSolver();
    
    m_world = new btDiscreteDynamicsWorld(m_collisionDispather, m_broadphase, m_constraintSolver, m_collisionConfiguration);
    m_world->setGravity(btVector3(0, -100, 0));
    
    m_groundShape = new btStaticPlaneShape(btVector3(0, 1, 0), 40);

    btTransform groundTransform;
    groundTransform.setIdentity();
    groundTransform.setOrigin(btVector3(0, -41, 0));
    m_groundBody = createRigidBody(0, groundTransform, m_groundShape);
}

bool RagDoll::stepSimulation(float amount)
{
    bool positionChanged = true;
    m_world->stepSimulation(btScalar(amount));
    
    if (m_boneBodies.empty())
        return positionChanged;
    
    std::vector<std::pair<QVector3D, QVector3D>> newBonePositions(m_boneBodies.size());
    m_setpJointNodeTree = m_jointNodeTree;
    for (const auto &it: m_boneShapes) {
        const auto *body = m_boneBodies[it.first];
        int jointIndex = it.second->getUserIndex();
        btTransform btWorldTransform;
        if (body->getMotionState()) {
            body->getMotionState()->getWorldTransform(btWorldTransform);
        } else {
            btWorldTransform = body->getWorldTransform();
        }
        const auto &btOrigin = btWorldTransform.getOrigin();
        QVector3D position = QVector3D(btOrigin.x(), btOrigin.y(), btOrigin.z());
        if (!qFuzzyCompare(position, m_boneLastPositions[it.first])) {
            positionChanged = true;
            m_boneLastPositions[it.first] = position;
        }
        float halfHeight = m_boneLengthMap[it.first] * 0.5;
        btVector3 oldBoneHead(btScalar(0.0f), btScalar(-halfHeight), btScalar(0.0f));
        btVector3 oldBoneTail(btScalar(0.0f), btScalar(halfHeight), btScalar(0.0f));
        btVector3 newBoneHead = btWorldTransform * oldBoneHead;
        btVector3 newBoneTail = btWorldTransform * oldBoneTail;
        newBonePositions[jointIndex] = std::make_pair(
            QVector3D(newBoneHead.x(), newBoneHead.y(), newBoneHead.z()),
            QVector3D(newBoneTail.x(), newBoneTail.y(), newBoneTail.z())
        );
    }
    
    QVector3D newRootBoneMiddle = (newBonePositions[0].first + newBonePositions[0].second) * 0.5;
    QVector3D modelTranslation = newRootBoneMiddle - m_boneMiddleMap[Rigger::rootBoneName];
    qDebug() << "modelTranslation:" << modelTranslation << "newRootBoneMiddle:" << newRootBoneMiddle << "oldRootBoneMiddle:" << m_boneMiddleMap[Rigger::rootBoneName];
    m_setpJointNodeTree.addTranslation(0, modelTranslation);
    
    std::vector<QVector3D> directions(newBonePositions.size());
    for (size_t index = 0; index < newBonePositions.size(); ++index) {
        const auto &bone = m_bones[index];
        directions[index] = (bone.tailPosition - bone.headPosition).normalized();
    }
    std::function<void(size_t index, const QQuaternion &rotation)> rotateChildren;
    rotateChildren = [&](size_t index, const QQuaternion &rotation) {
        const auto &bone = m_bones[index];
        for (const auto &childIndex: bone.children) {
            directions[childIndex] = rotation.rotatedVector(directions[childIndex]);
            rotateChildren(childIndex, rotation);
        }
    };
    for (size_t index = 0; index < newBonePositions.size(); ++index) {
        QQuaternion rotation;
        const auto &oldDirection = directions[index];
        QVector3D newDirection = (newBonePositions[index].second - newBonePositions[index].first).normalized();
        rotation = QQuaternion::rotationTo(oldDirection, newDirection);
        m_setpJointNodeTree.updateRotation(index, rotation);
        rotateChildren(index, rotation);
    }
    
    m_setpJointNodeTree.recalculateTransformMatrices();
    return positionChanged;
}

const JointNodeTree &RagDoll::getStepJointNodeTree()
{
    return m_setpJointNodeTree;
}

btRigidBody *RagDoll::createRigidBody(btScalar mass, const btTransform &startTransform, btCollisionShape *shape)
{
    bool isDynamic = !qFuzzyIsNull(mass);

    btVector3 localInertia(0, 0, 0);
    if (isDynamic)
        shape->calculateLocalInertia(mass, localInertia);

    btDefaultMotionState *myMotionState = new btDefaultMotionState(startTransform);

    btRigidBody::btRigidBodyConstructionInfo rbInfo(mass, myMotionState, shape, localInertia);
    btRigidBody *body = new btRigidBody(rbInfo);

    m_world->addRigidBody(body);

    return body;
}