/* Several threads making their first call at once must all get the
 * interpreter started exactly once (std::call_once in the linkstub). */
extern int answer_race();
