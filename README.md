# 현재 상태

> ⚠️ 현재 프로젝트는 PoC(Proof of Concept) 단계입니다.

현재 구현된 기능

- Unity ParticleSystem JSON Import
- Niagara System 생성
- Niagara Emitter 생성
- Sprite Renderer 생성
- Texture 자동 검색
- 기본 Material Instance 생성
- 기본 Emitter 구조 생성

현재는 **Unity ParticleSystem을 Niagara로 생성하는 것**까지는 가능하지만, 원본과 동일한 품질의 이펙트를 재현하지는 못합니다.

---

# 현재 부족한 점

## Niagara 생성

- [ ] 수동으로 생성한 Niagara와 동일한 Graph 구조 생성
- [ ] Factory로 생성한 Emitter의 기본 구성 분석
- [ ] 생성된 Niagara의 시각적 품질 개선

---

## Main Module

- [x] Lifetime
- [x] Start Size
- [x] Start Rotation
- [ ] Speed 변환 정확도 개선
- [ ] Simulation Space 검증

---

## Emission

- [x] Burst 정보 읽기
- [ ] Burst 타이밍 정확도
- [ ] Spawn Rate 지원

---

## Shape

- [ ] Cone
- [ ] Sphere
- [ ] Box
- [ ] Mesh Shape

---

## Color

- [ ] Color Over Lifetime
- [ ] Alpha Over Lifetime
- [ ] Gradient 변환

---

## Size

- [ ] Size Over Lifetime
- [ ] Curve 변환

---

## Velocity

- [ ] Velocity Over Lifetime

---

## Force

- [ ] Force Over Lifetime

---

## Noise

- [ ] Noise Module

---

## Collision

- [ ] Collision Module

---

## Trails

- [ ] Trail Renderer

---

## Texture Sheet Animation

- [ ] Flipbook Animation
- [ ] Frame Blending
- [ ] Cycle 처리

---

# 시각적 품질

현재 생성되는 Niagara는 구조 검증을 위한 수준입니다.

아래 항목은 아직 개선이 필요합니다.

- Sprite 크기 보정
- Curve 정확도
- Material 표현
- Renderer 설정
- Blend Mode
- Sorting
- Sprite Alignment
- Pivot 보정

---

# 향후 목표

최종 목표는 Unity ParticleSystem을 100% 동일하게 재현하는 것이 아닙니다.

목표는 다음과 같습니다.

Unity ParticleSystem
→ JSON Export
→ Niagara Import
→ 수정 가능한 Niagara System 생성

즉, 사람이 처음부터 만드는 시간을 줄여주는 **초기 제작 도구**를 목표로 합니다.

---

# 현재 가장 큰 문제

현재 Factory를 이용해 생성한 Niagara는 정상적으로 생성되지만,
수동으로 제작한 Niagara와 비교했을 때 시각적인 품질 차이가 매우 큽니다.

원인 후보

- Factory 생성 방식의 한계
- 기본 Graph 구성 차이
- Module 설정 차이
- Parameter Binding 차이
- Renderer 초기 설정 차이

해당 부분은 앞으로 가장 우선적으로 분석 및 개선할 예정입니다.

# 현재 고민 중인 방향

현재는 Factory를 이용하여 Niagara를 직접 생성하는 방식을 사용하고 있습니다.

다만 생성된 Niagara와 수동으로 제작한 Niagara의 품질 차이가 상당히 큰 것을 확인했습니다.

따라서 향후 아래 두 가지 방향을 비교 검토할 예정입니다.
