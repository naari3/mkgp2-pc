/* C++ functions the sources declare and the original never needed as code.
 *
 * FlowFader (include/game/FlowScene.h) has a virtual destructor that is
 * declared only: under MWCC the derived classes' destructors contain the
 * (empty) base part inline. A host compiler calls the base-object
 * destructor, which has nothing to do. */
void mkgp2pc_FlowFader_base_dtor(void* self) __asm__("_ZN9FlowFaderD2Ev");
void mkgp2pc_FlowFader_base_dtor(void* self) { (void)self; }
