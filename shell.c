#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>

#define STREQ(A,B) (strcmp((A),(B)) == 0)
#define VALIDJOBID(C) (((C) >= 1) && ((C) <= NUM_JOBS))

#define ARG_LEN 100
#define ARG_NUM 10
#define PATH_SIZE 200
#define NUM_JOBS 100

#define SETTING_BACKGROUND 0
#define SETTING_FOREGROUND 1

#define STATUS_RUNNING 0
#define STATUS_STOPPED 1
#define STATUS_TERMINATED 2
#define STATUS_COMPLETE 3

typedef struct Job {
   char** command;
   int num_args;
   pid_t pid;
   int setting;
   int status;
} Job;

Job* jobs[NUM_JOBS];

/* Display Methods */

void printArgs(Job* job) {
   for (int i = 0; i < job->num_args; i++) printf("%s ", job->command[i]);
   printf("\n");
}

void printJob(int job_id) {
   Job* job = jobs[job_id-1];
   if (job && (job->status <= STATUS_STOPPED)) {
      printf("[%d] %d ", job_id, job->pid);

      if (job->status == STATUS_RUNNING) printf("Running");
      else if (job->status == STATUS_STOPPED) printf("Stopped");

      for (int k = 0; k < job->num_args; k++) printf(" %s", job->command[k]);
      if (job->setting == SETTING_BACKGROUND) printf(" &");
      printf("\n");
   }
}

/* Job List Methods */

int getNextJobId() {
   for (int i = 0; i < NUM_JOBS; i++) {
      if (jobs[i] == NULL) { return i+1; }
   }
   return -1;
}

//    Insert Methods

int insertJob(Job* new_job) {
   int idx = getNextJobId();
   if (idx != -1) {
      jobs[idx-1] = new_job;
      return 0;
   }
   return -1;
}

//    Getter Methods

int findJobIdByPID(pid_t pid) {
   for (int i = 0; i < NUM_JOBS; i++) {
      if (jobs[i] && (jobs[i]->pid == pid)) return i+1;
   }
   return -1;
}

Job* findJobByPID(pid_t pid) {
   int job_id = findJobIdByPID(pid);
   if (job_id > 0) return jobs[job_id-1];
   return NULL;
}

//    Setter Methods

int setJobStatusByPID(pid_t pid, int new_status) {
   Job* job = findJobByPID(pid);
   if (job != NULL) { job->status = new_status; return 0; }
   return -1;  
}

//    Deallocation Methods

void freeArgs(char** args, int num_args) {
   if (args) {
      for (int i = num_args-1; i >= 0; i--) free(args[i]);
      free(args);
   }
}

void freeJob(Job* job) {
   if (job) {
      if (job->command) freeArgs(job->command, job->num_args);
      free(job);
   }
}

void freeAllJobs() {
   for (int i = 0; i < NUM_JOBS; i++) { 
      freeJob(jobs[i]); 
      jobs[i] = NULL; 
   }
}

int dequeJob(int job_id) {
   if (VALIDJOBID(job_id) && (jobs[job_id-1])) {
      freeJob(jobs[job_id-1]);
      jobs[job_id-1] = NULL;
      return 0;
   }
   return -1;
}

/* Process Management */

int checkJobStatus(int job_id, int status) {
   Job* job = jobs[job_id-1];
   if (job) {
      return (job->status == status) ? 1 : 0;
   }
   return -1;
}

int waitForPID(int pid) {
   int retcode = 0;
   waitpid(pid, &retcode, WUNTRACED);
    
   if (WIFEXITED(retcode)) {
      setJobStatusByPID(pid, STATUS_COMPLETE);
   } else if (WIFSIGNALED(retcode)){
      setJobStatusByPID(pid, STATUS_TERMINATED);
   } else if (WSTOPSIG(retcode)) {
      retcode = -1;
      setJobStatusByPID(pid, STATUS_STOPPED);
   }

   return retcode;
}

void reapChildren() {
   int pid = -1;
   int retcode = 0;

   while ((pid = waitpid(-1, &retcode, WNOHANG | WUNTRACED | WCONTINUED)) > 0) {     
      int target_jobid = findJobIdByPID(pid);
      if ((VALIDJOBID(target_jobid)) && jobs[target_jobid-1]) {
         if (WIFEXITED(retcode)) {
            jobs[target_jobid-1]->status = STATUS_COMPLETE;
         } else if (WIFSTOPPED(retcode)){
            jobs[target_jobid-1]->status = STATUS_STOPPED;
         } else if (WIFCONTINUED(retcode)) {
            jobs[target_jobid-1]->status = STATUS_RUNNING;
         }
      
         // Remove job if completed
         if (jobs[target_jobid-1]->status > STATUS_STOPPED) { // Terminated or complete
            dequeJob(target_jobid);
         }
      }
   }
}

/* Builtin Commands */

int bg(Job* job) { 
   if (job->num_args != 2) return 0;

   int job_id = atoi(job->command[1] + 1);
   if (!(VALIDJOBID(job_id))) return 0;
   Job* target_job = jobs[job_id-1];

   if (target_job) {
      pid_t pid = target_job->pid;
      
      if (kill(-pid, SIGCONT) >= 0) {
         target_job->status = STATUS_RUNNING;
         target_job->setting = SETTING_BACKGROUND;       
      } 
   }

   return 0;
}

int fg(Job* job) {
   if (job->num_args != 2) return 0;

   int job_id = atoi(job->command[1] + 1);
   if (!(VALIDJOBID(job_id))) return 0;
   Job* target_job = jobs[job_id-1];

   if (target_job) {
      pid_t pid = target_job->pid;

      if (kill(-pid, SIGCONT) >= 0) {
         tcsetpgrp(0, pid);
         target_job->status = STATUS_RUNNING;
         target_job->setting = SETTING_FOREGROUND;
         if (waitForPID(pid) == 0) {
            freeJob(jobs[job_id-1]);
            jobs[job_id-1] = NULL;
         }
         
         signal(SIGTTOU, SIG_IGN);
         tcsetpgrp(0, getpid());
         signal(SIGTTOU, SIG_DFL);
      }
   }
   return 0;
}

int killJob(Job* job) {
   if (job->num_args != 2) return 0;   

   Job* target_job = NULL;
   int target_jobid = atoi(job->command[1] + 1);
   if (VALIDJOBID(target_jobid)) target_job = jobs[target_jobid-1];
   
   if (target_job) {
      pid_t target_pid = target_job->pid; 
      kill(target_pid, SIGCONT);
      if (kill(target_pid, SIGTERM) == 0) {
         setJobStatusByPID(target_pid, STATUS_TERMINATED);
         int retcode = waitForPID(-target_pid);
         if (retcode != 0) {
            freeJob(target_job);
            jobs[target_jobid - 1] = NULL;
            printf("[%d] %d terminated by signal %d\n", target_jobid, target_pid, SIGTERM);
         }
      }
   }
   
   return 0;
}

int quit = 0;

int exitShell(Job* job) {
   quit = 1;
   return quit;
}

int cd(Job* job) {
   if (job->num_args > 2) return 0;

   char* new_path = (job->num_args > 1) ? job->command[1] : getenv("HOME");
   if (chdir(new_path) == 0) {
      char current_dir[PATH_SIZE];
      getcwd(current_dir, sizeof(char) * PATH_SIZE);
      setenv("PWD", current_dir, 1); 
   }
   return 0;
}

int printAllJobs(Job* job) {
   for (int i = 0; i < NUM_JOBS; i++) {
      printJob(i+1);
   } 
   return 0;
}

char* builtin_commands[] = {"bg", "fg", "exit", "cd", "jobs", "kill"};

int checkBuiltin(Job* job) {
   if (!job || (job->num_args < 1)) return -2; // job is empty
   for (int i = 0; i < 6; i++) {
      if (STREQ(job->command[0], builtin_commands[i])) return i; // job is builtin function
   }
   return -1; // job is not builtin
}

int runBuiltin(Job* job) {
  int (*builtin_funcs[]) (Job* job) = { bg, fg, exitShell, cd, printAllJobs, killJob };
   for (int i = 0; i < 6; i++) {
      if (STREQ(job->command[0], builtin_commands[i])) {
         (*builtin_funcs[i])(job);
         return 0;
      }
   }
   return -1;
}

/* Main Input and Execution */

char* concat_strings(char* s1, char* s2) {
   int new_size = (strlen(s1) + strlen(s2) + 1);
   char *s3 = (char*) malloc(sizeof(char) * new_size);
   strcpy(s3, s1);
   strcat(s3, s2);
   return s3;
}

void handle_cz(int sig) {
   printf("\n> ");
   fflush(stdout);
}

void handle_chld(int sig) {
   pid_t pid;
   int status;
   while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
      int job_id = findJobIdByPID(pid);
      if (VALIDJOBID(job_id)) {   
         printf("[%d] %d terminated by signal %d\n", job_id, pid, status);
         dequeJob(job_id);
      }
      kill(-pid, SIGKILL);
   }
}

int getCommandInLocation(Job* job, char* argnames[]) {
   argnames[0] = (char*) NULL;
   for (int i = 1; i < job->num_args; i++) {
      argnames[i] = (char*) malloc(sizeof(char)*(strlen(job->command[i]) + 1));
      strcpy(argnames[i], job->command[i]);
   }
   argnames[job->num_args] = (char*) NULL;
   
   char* executable = job->command[0];
   int isPath = ((executable[0] == '/') || (executable[0] == '.'));
   if (isPath){
      if (access(executable, X_OK) == 0) argnames[0] = executable;  
   } else { 
      char* cwd = concat_strings(getenv("PWD"), "/");
      char* local_path = concat_strings(cwd, executable);
      free(cwd);
      if (access(local_path, X_OK) == 0) { 
         argnames[0] = local_path;
      } else { 
         free(local_path);
         char* usrBin = concat_strings("/usr/bin/", executable); 
         if (access(usrBin, X_OK) == 0) { argnames[0] = usrBin; }
         else { 
            free(usrBin);   
            char *globalBin = concat_strings("/bin/", executable);
            if (access(globalBin, X_OK) == 0) { argnames[0] = globalBin; }
            else { free(globalBin); }
         }
      } 
   }
   
   //printf("%d\n", argnames[0] != NULL);
   if (argnames[0] != NULL) { return 0; }
   return (isPath ? 1 : 2);
}

int execute(Job* job) {
   reapChildren();
   int job_type = checkBuiltin(job);
   if (job_type >= 0) {
      runBuiltin(job);
      freeJob(job);
   } else if (job_type == -1) {
      insertJob(job);
     
      pid_t pid = fork();
      if (pid == 0) { // In child process
         // Reset all signal to default settings
         signal(SIGINT , SIG_DFL);
         signal(SIGTSTP, SIG_DFL);
         signal(SIGTTIN, SIG_DFL);
         signal(SIGTTOU, SIG_DFL);
         signal(SIGCHLD, SIG_DFL);
         signal(SIGQUIT, SIG_DFL);

         job->pid = getpid();
         setpgid(0, job->pid);
         job->status = STATUS_RUNNING;
         char** cmdInLocation = (char**) malloc(sizeof(char*) * (job->num_args + 1));
         int execFound = getCommandInLocation(job, cmdInLocation);
         if (execFound == 0) {
            execvp(cmdInLocation[0], cmdInLocation); 
         } else { 
            if (execFound == 1) // path
               printf("%s: No such file or directory\n", job->command[0]);
            else if (execFound == 2) // not path
               printf("%s: command not found\n", job->command[0]);
            freeArgs(cmdInLocation, job->num_args+1);
            int job_id = findJobIdByPID(getpid());
            if (VALIDJOBID(job_id)) {
               freeJob(jobs[job_id-1]);
               jobs[job_id-1] = NULL;
            }
         }
         exit(0);
      } else {
         // In parent process
         job->pid = pid;
         job->status = STATUS_RUNNING;
         setpgid(pid, pid);
         if (job->setting == SETTING_FOREGROUND) {
            tcsetpgrp(0, job->pid);
            int retcode = waitForPID(pid);
            signal(SIGTTOU, SIG_IGN);
            tcsetpgrp(0, getpid());
            signal(SIGTTOU, SIG_DFL);
            
            if (retcode != -1) {
               int child_jobid = findJobIdByPID(pid);   
               freeJob(jobs[child_jobid-1]);
               jobs[child_jobid-1] = NULL;
               if (WIFSIGNALED(retcode)) printf("\n[%d] %d terminated by signal %d\n", child_jobid, pid, retcode);
            } else {
               printf("\n");
            }
         } else {
            printf("[%d] %d\n", findJobIdByPID(pid), pid);
         }
      }
   } else if (job_type == -2) {
      freeJob(job);
   }

   return job_type;
}

Job* createJobFromInput(int* quit) {
   char* argnames[ARG_NUM];
   for (int i = 0; i < ARG_NUM; i++) argnames[i] = (char*) NULL;
   
   char x;
   int argpos = 0;
   int argc = 0;
   int fg_flag = 1;
   int endArg = 0;
   int endInput = 0; 
   do {
      endArg = 0; 
      endInput = 0;
      x = getchar();
      if (x == ' ') { endArg = 1; }
      else if (x == '\n') { endArg = 1; endInput = 1; }
      else if (x == EOF) { endArg = 1; endInput = 1; *quit = 1; }
      else if (x == '&') { 
         endArg = 1; endInput = 1; fg_flag = 0; 
      }
      else {
         if (argpos == 0) argnames[argc] = (char*) malloc(sizeof(char) * ARG_LEN);
         argnames[argc][argpos] = x;
         argpos += 1;
      }
      if (endArg && (argpos != 0)) {
         argnames[argc][argpos] = '\0';
         argc += 1;
         argpos = 0;
      }
   } while (!endInput);  
   if (x == '&') {
      do {
         x = getchar();
      } while ((x != EOF) && (x != '\n'));
   }

   Job* job = (Job*) malloc(sizeof(Job));
   job->command = (char**) malloc(sizeof(char*) * argc);
   for (int i = 0; i < argc; i++) {
      job->command[i] = (char*) malloc(sizeof(char) * (strlen(argnames[i]) + 1));   
      strcpy(job->command[i], argnames[i]);
   }
   job->num_args = argc;
   job->pid = -1;
   job->setting = fg_flag;
   job->status = -1;
   
   for (int i = argc-1; i >= 0; i--) free(argnames[i]);
   return job;
}

int main() {
   // Process group setup
   pid_t main_pid = getpid();
   setpgid(main_pid, main_pid);
   tcsetpgrp(0, main_pid);
  
   // Signal setup
   signal(SIGINT, &handle_cz);
   signal(SIGQUIT, SIG_IGN);
   signal(SIGTSTP, &handle_cz);
   signal(SIGTTIN, SIG_IGN);
   signal(SIGCHLD, &handle_chld);

   freeAllJobs();

   Job* job = NULL;
   do {
      printf("> ");
      fflush(stdout);
      
      job = createJobFromInput(&quit);
      if (job->num_args > 0) { execute(job); }
      else {
         if (job) freeJob(job);
         reapChildren();
         continue;
      }
   } while (!quit);

   // Cleanup
   for (int i = 0; i < NUM_JOBS; i++) {
      if (jobs[i] && jobs[i]->command) {
         if (jobs[i]->pid > 0) {
            kill(-jobs[i]->pid, SIGHUP);
            if (jobs[i]->setting == SETTING_BACKGROUND) kill(-jobs[i]->pid, SIGCONT);
         }
      }
   }
   freeAllJobs();

   return 0;
}
