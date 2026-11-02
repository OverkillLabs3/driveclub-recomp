/* Only BB_CAMERA_PATCH is public. Fixed internal production dependency graph. */
static void camera_fix_bootstrap_configure(void){}
static const char*camera_fix_getenv(const char*name){
 const char*enabled=getenv("BB_CAMERA_PATCH");
 if(!strcmp(name,"BB_CAMERA_PATCH"))return enabled;
 if(!enabled||strcmp(enabled,"1"))return NULL;
 if(!strcmp(name,"BB_CAMERA_ACCURACY_ORACLE"))return "1";
 if(!strcmp(name,"BB_CAMERA_ANGULAR_INIT_SHARED_Q"))return "1";
 if(!strcmp(name,"BB_CAMERA_ANGULAR_REFERENCE"))return "1";
 if(!strcmp(name,"BB_CAMERA_HEAD_GROUP"))return "1";
 if(!strcmp(name,"BB_CAMERA_HEAD_LATE_Q"))return "1";
 if(!strcmp(name,"BB_CAMERA_HEAD_REAL_CLOCK"))return "1";
 if(!strcmp(name,"BB_CAMERA_HEAD_REFERENCE"))return "0";
 if(!strcmp(name,"BB_CAMERA_LF_REFERENCE"))return "1";
 if(!strcmp(name,"BB_CAMERA_LF_SHARED_Q_INPUT"))return "1";
 if(!strcmp(name,"BB_CAMERA_REFERENCE_REPLAY_FEEDBACK"))return "1";
 if(!strcmp(name,"BB_CAMERA_REFERENCE_SPRINGS"))return "1";
 if(!strcmp(name,"BB_CAMERA_TEMPORAL_GROUP_WRITE"))return "1";
 if(!strcmp(name,"BB_GFORCE_REFERENCE"))return "1";
 if(!strcmp(name,"BB_HEAD_CLOCK_SELECTOR"))return "1";
 if(!strcmp(name,"BB_HEAD_PAUSED_CLOCK"))return "1";
 if(!strcmp(name,"BB_HUD_SHARED_Q"))return "1";
 if(!strcmp(name,"BB_NATIVE_PACKET_CACHE"))return "1";
 if(!strcmp(name,"BB_PRIVATE_LATE_Q"))return "1";
 if(!strcmp(name,"BB_PRIVATE_LATE_Q_TOMBSTONE"))return "1";
 if(!strcmp(name,"BB_REPLAY_LINEAR_MEAN_REBASE"))return "1";
 if(!strcmp(name,"BB_REPLAY_LINEAR_NATIVE_Q"))return "1";
 if(!strcmp(name,"BB_REPLAY_PACKET_LIFECYCLE"))return "1";
 if(!strcmp(name,"BB_REPLAY_PACKET_LIFECYCLE_ROLES"))return "1";
 if(!strcmp(name,"BB_REPLAY_PAIRED_QUERY_Q"))return "1";
 return NULL;
}
