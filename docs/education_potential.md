# Educational Potential of the protoCore Ecosystem

## A Proposal for the Computer Science Programme at ITBA

This document outlines a proposal to integrate the protoCore ecosystem — a family of language runtimes built on a shared object-system core — into the Computer Science programme at ITBA, initially as a small directed-research pilot. The proposal is structured around a single thesis: that the most consequential pedagogical opportunity of the next five years is not *teaching about AI*, but rather *teaching engineering with AI agents as collaborators*, and that this requires real codebases of meaningful complexity, not toy exercises. The protoCore ecosystem, by virtue of how it was built and what it contains, is unusually well suited as that substrate.

The document is intended as material for a working meeting on implementation details. It assumes a reader who is fluent in higher-education curriculum design but has not previously encountered the project. It provides the technical, pedagogical, and operational context required to move directly to discussion of pilot structure, governance, and timelines.

---

## 1. The Moment in Computer Science Education

Universities across the world are in the early phase of an institutional crisis around generative AI and coding assistants. Three responses have become typical, and each is insufficient.

The first response is **defensive**: prohibit assistant use in coursework, develop AI-output detectors, treat assisted code as a form of plagiarism. This posture is widely understood to be unsustainable, both because the detection tools are unreliable and because graduates will enter workplaces in which assistant use is mandatory, not optional. Universities that maintain this posture risk producing graduates who reach industry with a serious skill gap on day one.

The second response is a **lightweight curricular addition**: an elective course titled "Introduction to AI" or "LLMs in Practice," typically a semester long, structured as a survey. These courses tend to be valuable as overviews but rarely produce durable practitioner skill. Students learn concepts; they do not develop the embodied judgment that comes from sustained work on real systems with assistants integrated into the daily workflow.

The third response is **letting students sort it out themselves**: tacitly allowing assistant use, providing no scaffolding, and trusting that students will develop the relevant capabilities on side projects. This produces graduates with very uneven skill profiles and abdicates the university's role in cultivating professional judgment.

What is missing from the current landscape — and what we propose to begin building at ITBA — is a fourth response: **agent-assisted engineering as a structured practice on real production-grade codebases, supervised, documented, and credit-bearing.** The technical skills, the methodological skills, and the professional judgement should be developed together, on systems where the assistance produces real consequences.

This is not an abstract aspiration. The capability gap it addresses is concrete: in industry today, the most valuable junior engineer is not the one who can write code from scratch most fluently, but the one who can direct, verify, and architect with assistance. The market signal is unambiguous; the educational response has not caught up. ITBA is well positioned to be the first Latin American institution to do so seriously.

---

## 2. The Project at a Glance

protoCore is a foundational object-system library written in C++20. It was designed around principles that diverge deliberately from mainstream language runtimes, and the resulting design is unusual enough to merit a brief description before discussing its educational use.

The core design principles are:

**Immutability by default.** Collections — lists, strings, tuples, sets, sparse lists — are immutable. Modifications return new versions through structural sharing, implemented internally with AVL trees and ropes. This is not a stylistic choice; it is the foundation that makes the next two principles possible.

**GIL-free concurrency.** protoCore uses native operating-system threads with no global interpreter lock. A dedicated concurrent garbage collector minimises stop-the-world pauses through concurrent marking. Because the data structures are immutable by default, threads can share them without copying, without synchronisation overhead, and without the contention that defines mainstream dynamic-language runtimes.

**Hardware-aware memory layout.** Cell allocations are aligned to 64-byte boundaries, matching CPU cache lines. Each thread has its own lock-free allocation arena. Small integers (up to 56 bits) are encoded inline in tagged pointers. These choices are not premature optimisation; they are the difference between a runtime that is concurrency-correct on paper and one that scales linearly on real hardware.

**Prototype-based object model.** protoCore uses Lieberman-style prototypes with dynamic inheritance. A single object model serves all the language frontends that have been built on top of it.

On this foundation, three language runtimes have been built:

- **protoJS**, a JavaScript runtime that replaces the native runtime of QuickJS while retaining its parser. It currently passes approximately 92.7% of the test262 conformance suite across the eighteen built-in categories examined to date. It includes an integrated profiler, a Chrome DevTools Protocol debugger, and a memory analyser.
- **protoPython**, a GIL-free Python 3.14 runtime with HPy FFI bindings for native extensions. Benchmark suites place it at roughly thirty times the performance of CPython 3.14 on representative workloads (geometric mean), though as with all such measurements the comparison depends heavily on workload selection.
- **protoST**, a Smalltalk-inspired environment that serves as a demonstrator of protoCore's potential as a substrate for higher-level dynamic languages. It is not intended as a commercial Smalltalk implementation but as a proof that the core model can host a third semantic family — message passing with actor-like delocked execution — without modification.

The shared runtime principle is the pedagogical headline: a single `ProtoObject` model serves, simultaneously, a JavaScript instance, a Python class, and a Smalltalk object. The students who work on this codebase will see, in concrete code, the proposition that **languages are interfaces over runtimes** — an idea that is asserted in every Programming Languages course but rarely visible in production form.

The codebase totals approximately 200,000 lines of C++20. It is publicly available under permissive licensing. Development has been conducted with extensive AI-agent assistance from the outset, documented openly in commit history, design notes, and decision logs. The system is described in its own README as Phase 6 Complete but explicitly **not production ready** — open for community review. This honesty matters for the proposal: we are not asking students to contribute to a commercial product, we are asking them to participate in a serious research-grade codebase that has real bugs, real architectural decisions, and real consequences.

---

## 3. Five Pillars of the Proposal

### 3.1 Agent-assisted engineering as curricular practice, not theory

The defining skill of the engineer graduating in 2027 will not be unaided code production. It will be the ability to direct, verify, and architect systems in collaboration with AI agents. That skill cannot be taught through a single elective course, nor acquired through Copilot use on short coursework exercises. It requires sustained work — months — on a codebase where the agent is integrated into the workflow and the student must take responsibility for what the collaboration produces.

protoCore offers exactly this. The codebase was built with agent assistance from the outset. Commits are signed with explicit AI co-authorship attribution. Design choices are written down and reviewable. Architectural debates are documented. A student who joins as a contributor is not starting from zero, and is not being asked to copy patterns; they are joining a months-long technical conversation between humans and agents, and being asked to sustain their part of that conversation with independent judgement.

This is what they will do, on their first day, in their first job in 2027. The practice formed during their final months of university — when they are surrounded by supervision and have time to develop habits — is disproportionately valuable. The student who arrives at their first job already fluent in this mode of work has months of head start over peers who acquired the same skills under production pressure.

### 3.2 The agent amplifies what is hard; it does not replace it

The most instructive aspect of working with agents on real codebases is the brutal clarity with which it exposes what the agent does not solve. An agent writes code quickly; it does not decide which architecture is appropriate. An agent executes mass refactorings; it does not know when a refactoring is premature. An agent finds locally correct fixes; it cannot reason about global model consistency.

protoCore is dense with decisions that the agent alone cannot make well. The immutability invariants, the concurrent garbage collector contracts, the cell alignment model, the cross-language consistency requirements — each of these is a domain where an apparently reasonable agent proposal will silently break the system. A student contributing to this codebase will, predictably, encounter at least one such case per week. Each encounter is a lesson in what senior engineering judgement actually consists of: noticing the smell, formulating the hypothesis, verifying the failure, designing the correction.

This is the substance behind the phrase "the agent does not replace the engineer." It cannot be taught by being said. It must be experienced, on real code, with real consequences. protoCore provides the conditions under which that experience accumulates rapidly and safely.

### 3.3 Structural immutability and real concurrency as technical substrate

The pedagogical value of the project is not exhausted by the agent-collaboration dimension. The technical content — what the student learns about runtimes, data structures, and concurrency — is genuinely rare and genuinely useful.

Structural sharing through persistent data structures (AVL trees, ropes, copy-on-write semantics) is one of the most important ideas in modern software engineering. It underlies Clojure, Scala, Elm, React's reconciliation algorithm, and the architecture of distributed databases such as Datomic. Students typically encounter it in passing during functional programming electives. They rarely build it. In protoCore, they would not only build it but would see the design consequences cascade through GC behaviour, threading semantics, and language frontend design.

GIL-free Python is the most discussed topic in dynamic-language engineering today. PEP 703 has been accepted, CPython 3.13 ships an experimental no-GIL build, and every major data and AI infrastructure team in the industry is evaluating migration. protoPython is GIL-free by design, not by retrofit. A student who works on it understands the problem space at a level that few practitioners reach without a research role.

These are the topics where graduates with depth are scarce per square kilometre in the industry. Combined with the agent-collaboration training, they form an unusually employable skill profile.

### 3.4 The final project can be a language, not an application

Most final projects in undergraduate Computer Science programmes are applications — a web platform, a mobile app, a recommendation system, a data pipeline. These are valuable but they signal incremental skill: more sophisticated versions of what the student already did in earlier coursework.

protoCore enables a categorically different kind of final project: **a language**. A motivated student, with adequate supervision and the assistance of agents on the implementation mechanics, can deliver a usable protoClojure, a protoScheme, a protoLua, or a comparable system in one or two semesters. The runtime is already in place. The student supplies the front-end, the semantic mapping, and the integration. The work is substantial — it is real research-grade engineering — but it is no longer infeasible at the undergraduate level.

Other comparable projects include a TypeScript-style structural type checker for protoJS, a visual profiler or debugger for the runtime, an optimisation pass (inline caching, escape analysis), a WebAssembly playground for protoST, or a substantive contribution to test262 conformance — a measurable target whose progress is publicly trackable.

The professional consequence is significant. A curriculum vitae stating "I implemented a functional language on top of an open-source GIL-free runtime, with AI-assistance methodology documented in commit history" reads very differently from a curriculum vitae stating "I built a delivery application in React." The first signals the kind of engineer who will be hired in 2027; the second signals the kind of engineer who could have been hired in 2018.

### 3.5 Institutional positioning at a moment of inflection

Universities globally are aware that AI integration in engineering education is a strategic question. Few have a clear answer. Many are reacting defensively or with shallow electives. To our knowledge, **no university in Latin America has yet formalised agent-assisted engineering as structured curricular practice on a live, non-trivial research codebase.**

ITBA has the opportunity to be the first. Not as a publicity gesture but as a real institutional commitment that will be visible to prospective students, to industry partners, and to the academic community. The proposed pilot is small enough to be reversible, but the framing — "ITBA is where you go to learn engineering with agents on real systems" — has compounding value across admissions, partnerships, alumni placement, and academic visibility.

Publishable outcomes are plausible. The methodology — using a documented AI-assisted codebase as a substrate for undergraduate research training — is itself novel enough to be presentable at venues such as SPLASH (SIGPLAN's annual Onward!/PLATEAU education tracks), the SIGCSE technical symposium, the Annual Software Engineering Education Conference, and analogous Latin American venues. Three years from now, this could be the case study other institutions cite.

---

## 4. What Students Would Actually Build

The following is an indicative menu of projects, ranked by approximate difficulty and time investment. The intent is to illustrate the range available, not to prescribe specific assignments.

**Tier 1 — Bug fixes and conformance contributions (4–8 weeks each).** Targeted improvements to test262 conformance, documented as PRs against protoJS or protoPython. Each contribution is small, reviewable, and produces a measurable jump in a public conformance metric. These are ideal entry points for a student adjusting to the codebase and to the agent-assisted workflow.

**Tier 2 — Tooling and instrumentation (one semester).** A visual profiler for protoCore, a Chrome DevTools-style memory analyser for protoPython, a benchmark harness comparing protoJS with V8 across representative workloads, or a documentation generator that extracts design decisions from commit metadata.

**Tier 3 — Type systems and static analysis (one to two semesters).** A structural type checker for protoJS in the style of TypeScript, restricted to a clearly defined subset of the language. An escape analysis pass for protoPython to enable stack-allocation optimisations. A lint and code-style tool for protoST.

**Tier 4 — New language frontends (two semesters or final project).** protoClojure (persistent data structures map naturally onto protoCore's collection semantics). protoScheme or protoRacket (simpler semantics, ideal for ambitious second-year students). protoLua (minimal syntax, clear semantics, manageable scope). A Scala-subset frontend would be a substantial final-project undertaking but is tractable for the strongest students.

**Tier 5 — Runtime research contributions (two semesters or thesis).** Improvements to the concurrent garbage collector. Inline caching for the dispatch hot path. Cross-thread immutable sharing optimisations. Work in this tier crosses into publishable territory and is appropriate for students considering graduate study.

In each case, the agent assists with mechanics. The student supplies intent, design judgement, verification, and architectural coherence. The contribution flow — branch, commit, document, code review, merge — mirrors industry practice rather than approximating it.

---

## 5. Proposed Pilot Structure

The recommendation is to begin with a small, time-bounded pilot under existing institutional mechanisms, rather than designing a new course or programme structure up front.

**Format.** Directed research or supervised practical work, using whichever existing instrument best matches ITBA's administrative categories. The choice should optimise for time-to-start, not for ambition. A new elective course would require curriculum-committee approval and would delay the pilot by at least one academic cycle.

**Cohort size.** Three students. This number is small enough that genuine mentorship is feasible. It is large enough that the loss of one student to attrition does not collapse the pilot. It is not so large as to imply institutional commitment beyond what a pilot warrants.

**Duration.** One semester, with explicit checkpoints at the midpoint and at conclusion.

**Prerequisites.** Completion of, or current enrolment in, Compilers and Concurrency (or institutional equivalents). A minimum academic-standing threshold to be defined jointly with the academic supervisor. The selection should be by curated invitation from informatics faculty who know the students, not by open call.

**Supervision structure.** Two supervisors. The first is the project lead from the protoCore side, who provides technical depth, code review, and methodological guidance on agent-assisted engineering. The second is a faculty member from the Computer Science programme, who provides academic supervision, evaluates final reports, and ensures alignment with institutional requirements. The dual structure protects all parties and avoids any appearance of an external mentor making decisions properly belonging to the faculty.

**Time commitment.** Project lead: approximately six hours per week, including individual mentoring sessions, group code-review meetings, and asynchronous review. This number should be honestly assessed; under-committing here is the most reliable way to ensure pilot failure.

**Success criteria.** Defined and documented before the pilot starts. Indicative criteria: each student produces at least one merged contribution to the public codebase; each student documents their methodology in a written report suitable for academic evaluation; the cohort collectively delivers at minimum a measurable improvement in one publicly visible metric (test262 conformance, benchmark performance, or comparable). Anything beyond these baselines is a positive outcome.

**Exit clause.** Explicit. If the pilot does not produce the intended outcomes, it is closed at the end of the semester without renewal, with no implicit commitment to continuation. This protects both the institution and the project lead.

**Intellectual property.** Code produced by students is released under the existing project licence (permissive). Students retain authorship attribution. Published academic work carries ITBA as institutional affiliation and includes both supervisors as co-authors where their contribution justifies it. No copyright assignment to ITBA is requested or proposed.

---

## 6. Risk Framework

A proposal of this nature must anticipate, not avoid, the questions that responsible academic leadership will raise.

**What happens if the project lead becomes unavailable?** The codebase is public and permissively licensed. Student contributions are merged into the public repository and remain accessible regardless of the lead's continuing involvement. The faculty supervisor can continue to support academic aspects of the work. Continuity of deep technical mentorship cannot be guaranteed; this should be made explicit, and the pilot should be sized accordingly.

**What if student quality is uneven?** The pilot is small enough that one underperforming participant does not destabilise the cohort. The curated-invitation selection process is designed to minimise mismatch risk. The mid-semester checkpoint provides an opportunity to redirect or de-scope an individual project.

**Does this produce real employability gains?** The answer is yes, but indirectly. The proposal does not certify vocational skills; it produces engineers with unusual depth in two areas (runtimes and agent-assisted engineering) that the market is currently bidding for. Graduates of the pilot will have publicly visible contributions, demonstrable methodology, and a credentialing position that few peers in the region can match.

**Does this conflict with the rest of the Computer Science curriculum?** No. The pilot is structured as supplementary, not substitutive. Students continue all required coursework. The pilot complements existing instruction in Programming Languages, Concurrency, and Software Engineering without displacing it.

**What if a student commercialises their work?** The licensing framework permits this. The proposal explicitly does not condition student work on assignment of commercial rights to ITBA or to the project. Should commercial outcomes emerge, they are the student's to pursue; the educational arrangement does not pre-empt them.

**What if AI assistance produces work the student cannot defend?** This is a meaningful concern and the answer is the central pedagogical point. Each student deliverable includes a written reflection on the design decisions taken, the agent interactions that informed them, and the verification methodology applied. Oral defence at the semester checkpoint and conclusion provides the mechanism by which faculty supervision verifies that the student owns and understands the work. This is the same mechanism used for postgraduate research; it scales appropriately to this pilot.

---

## 7. Why ITBA, Why Now

The window for first-mover positioning in agent-assisted engineering education is open and will close within two to three years. By that horizon, most leading universities will have institutional positions on this question. The institutions that move now will define the practice; those that move later will adopt frameworks defined elsewhere.

ITBA has the structural advantages required to move first. The institution is small enough that a directed-research pilot can be approved without the multi-committee delays typical of larger universities. The Computer Science programme is technically strong, with sufficient depth in compilers and concurrency to evaluate the work seriously. The institutional brand benefits from association with serious technical practice rather than from following industry trends.

The pilot itself is small, reversible, and inexpensive. It does not require new courses, new infrastructure, or new administrative categories. It requires a director's decision, the identification of a co-supervisor from the Computer Science faculty, and the recruitment of three appropriately prepared students. These are decisions that can be made in a single working meeting.

---

## 8. Closing: Meeting as Working Session

This document is intended to render the meeting itself a working session on implementation details rather than an introductory presentation. The framing, the technical context, the pedagogical thesis, and the structural recommendations are set out above. What remains is the joint design of pilot specifics:

- Identification of the co-supervisor from the Computer Science faculty.
- Choice of administrative instrument (directed research, supervised practical work, or equivalent).
- Calendar: which semester the pilot begins, and how it is announced internally.
- Selection process: which faculty members are invited to recommend candidate students.
- Mid-semester and end-of-semester checkpoints, including assessment criteria.
- The internal communication strategy: how the pilot is presented to the Computer Science faculty.
- The exit clause and the conditions under which the pilot would or would not be renewed.

These are the questions for which a working meeting is genuinely necessary. The intent is to leave that meeting with a one-page programme description, signed by both parties, that can be put into administrative motion the following week.

---

*The protoCore ecosystem and its associated runtimes (protoJS, protoPython, protoST) are openly available under permissive licensing. Technical documentation, design notes, and development history are publicly accessible. This proposal is offered in the spirit of an institutional collaboration in which both parties carry real responsibility for outcomes.*
